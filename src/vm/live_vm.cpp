// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "dartplant/advanced/live_vm.h"

#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "runtime/runtime_internal.h"
#include "vm/runtime_profiles.h"

namespace dartplant {
namespace {

constexpr uint64_t kMaxObjectPoolEntries = 1ULL << 24;
constexpr uint64_t kMaxClassFunctions = 1ULL << 20;
constexpr size_t kLiveVmProfileV1Size =
    offsetof(DartPlantLiveVmProfile, code_unchecked_entry_point_offset);
constexpr size_t kLiveVmProbeInfoV1Size = offsetof(DartPlantLiveVmProbeInfo, requested_entry_kind);

template <typename T>
void CopyOutputPrefix(const T& source, T* destination) {
    const size_t caller_size = destination->struct_size;
    const size_t written_size = std::min(caller_size, sizeof(T));
    std::memcpy(destination, &source, written_size);
    destination->struct_size = static_cast<uint32_t>(written_size);
}

const CanonicalBoolLayout* FindCanonicalBoolLayout(uint32_t profile_version) {
    const RuntimeProfileRecord* profile = FindRuntimeProfileByVersion(profile_version);
    return profile == nullptr ? nullptr : &profile->canonical_bool;
}

const FunctionTypeLayout* FindFunctionTypeLayout(uint32_t profile_version) {
    const RuntimeProfileRecord* profile = FindRuntimeProfileByVersion(profile_version);
    return profile == nullptr ? nullptr : &profile->function_type;
}

const RawObjectLayout* FindRawObjectLayout(uint32_t profile_version) {
    const RuntimeProfileRecord* profile = FindRuntimeProfileByVersion(profile_version);
    return profile == nullptr ? nullptr : &profile->raw_object;
}

uint64_t MaxCidCount(const DartPlantLiveVmProfile& profile) {
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr || raw->class_id_tag_bits == 0 || raw->class_id_tag_bits >= 63) return 0;
    return uint64_t{1} << raw->class_id_tag_bits;
}

bool DecodeFunctionKind(const DartPlantLiveVmProfile& profile, uint32_t kind_tag,
                        uint32_t* out_kind) {
    const RuntimeProfileRecord* runtime_profile =
        FindRuntimeProfileByVersion(profile.profile_version);
    if (out_kind == nullptr || runtime_profile == nullptr ||
        runtime_profile->function_kind.tag_bits == 0 ||
        runtime_profile->function_kind.tag_bits >= 32 ||
        runtime_profile->function_kind.tag_shift >= 32 - runtime_profile->function_kind.tag_bits) {
        return false;
    }
    const uint32_t mask = (uint32_t{1} << runtime_profile->function_kind.tag_bits) - 1;
    *out_kind = (kind_tag >> runtime_profile->function_kind.tag_shift) & mask;
    return true;
}

// compiler::target::kNumParameterFlagsPerElement for ARM64. Each named parameter
// currently contributes one flag bit, so one compressed Smi stores 16 entries.
constexpr uint32_t kNamedParameterFlagsPerSmi = 16;

struct MemoryRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;
};

class ProcessMemoryReader final {
public:
    bool Refresh() {
        ranges_.clear();
        std::ifstream maps("/proc/self/maps");
        if (!maps) return false;
        std::string line;
        while (std::getline(maps, line)) {
            unsigned long long begin = 0;
            unsigned long long end = 0;
            char permissions[5] = {};
            if (std::sscanf(line.c_str(), "%llx-%llx %4s", &begin, &end, permissions) != 3 ||
                permissions[0] != 'r' || begin >= end) {
                continue;
            }
            ranges_.push_back({static_cast<uintptr_t>(begin), static_cast<uintptr_t>(end)});
        }
        std::sort(ranges_.begin(), ranges_.end(),
                  [](const MemoryRange& left, const MemoryRange& right) {
                      return left.begin < right.begin;
                  });
        return !ranges_.empty();
    }

    bool Contains(uintptr_t address, size_t size) const {
        if (size == 0) return true;
        if (address > std::numeric_limits<uintptr_t>::max() - size) return false;
        const uintptr_t end = address + size;
        auto it = std::upper_bound(
            ranges_.begin(), ranges_.end(), address,
            [](uintptr_t value, const MemoryRange& range) { return value < range.begin; });
        if (it == ranges_.begin()) return false;
        --it;
        return address >= it->begin && end <= it->end;
    }

    template <typename T>
    bool Read(uintptr_t address, T* out_value) const {
        // Keep the raw pointer precondition explicit here instead of relying on
        // Contains() to imply it. Besides being the correct contract for a
        // process-memory reader, this lets clang's path-sensitive analyzer
        // prove that memcpy never receives a null source pointer.
        if (address == 0 || out_value == nullptr || !Contains(address, sizeof(T))) return false;
        std::memcpy(out_value, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }

    bool ReadBytes(uintptr_t address, void* output, size_t size) const {
        if (address == 0 || output == nullptr || !Contains(address, size)) return false;
        std::memcpy(output, reinterpret_cast<const void*>(address), size);
        return true;
    }

    // Managed Dart heap mappings can change after /proc/self/maps is sampled.
    // process_vm_readv() turns that race into EFAULT/short-read instead of a
    // synchronous SIGSEGV in diagnostic APIs that inspect volatile VM memory.
    bool ReadSafely(uintptr_t address, void* output, size_t size) const {
        if (address == 0 || output == nullptr || !Contains(address, size)) return false;
#if defined(__linux__) && defined(SYS_process_vm_readv)
        iovec local = {.iov_base = output, .iov_len = size};
        iovec remote = {.iov_base = reinterpret_cast<void*>(address), .iov_len = size};
        const long result = syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0);
        return result == static_cast<long>(size);
#else
        return false;
#endif
    }

    template <typename T>
    bool ReadSafely(uintptr_t address, T* out_value) const {
        return ReadSafely(address, out_value, sizeof(T));
    }

private:
    std::vector<MemoryRange> ranges_;
};

bool SameString(const char* left, const char* right) {
    return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

bool HasSnapshotFeature(const char* features, std::string_view expected) {
    if (features == nullptr || expected.empty()) return false;
    std::string_view remaining(features);
    while (!remaining.empty()) {
        const size_t separator = remaining.find(' ');
        const std::string_view token = remaining.substr(0, separator);
        if (token == expected) return true;
        if (separator == std::string_view::npos) break;
        remaining.remove_prefix(separator + 1);
    }
    return false;
}

bool IsHeapObject(const DartPlantLiveVmProfile& profile, uint64_t tagged) {
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    return raw != nullptr && (tagged & raw->smi_tag_mask) == raw->heap_object_tag;
}

uintptr_t Untag(const DartPlantLiveVmProfile& profile, uint64_t tagged) {
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    return raw == nullptr || tagged < raw->heap_object_tag
               ? 0
               : static_cast<uintptr_t>(tagged - raw->heap_object_tag);
}

uint64_t ObjectPoolOffsetFromIndex(const DartPlantLiveVmProfile& profile, uint32_t index) {
    // Dart ObjectPool::OffsetFromIndex(): element_offset(index) - kHeapObjectTag.
    // ObjectPool entries are native-word-sized even when Dart heap pointers are
    // compressed; the tagged-object payload stored in an entry remains a full
    // ObjectPtr-sized word.
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr || profile.object_pool_elements_offset < raw->heap_object_tag) return 0;
    return static_cast<uint64_t>(profile.object_pool_elements_offset) - raw->heap_object_tag +
           static_cast<uint64_t>(index) * sizeof(uint64_t);
}

bool ObjectPoolIndexFromOffset(const DartPlantLiveVmProfile& profile, uint64_t offset,
                               uint32_t* out_index) {
    if (out_index == nullptr) return false;
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr || profile.object_pool_elements_offset < raw->heap_object_tag) return false;
    const uint64_t first =
        static_cast<uint64_t>(profile.object_pool_elements_offset) - raw->heap_object_tag;
    if (offset < first) return false;
    const uint64_t relative = offset - first;
    if ((relative % sizeof(uint64_t)) != 0) return false;
    const uint64_t index = relative / sizeof(uint64_t);
    if (index > std::numeric_limits<uint32_t>::max()) return false;
    *out_index = static_cast<uint32_t>(index);
    return true;
}

// Android arm64 enables top-byte-ignore pointer tagging for native heap
// allocations. Dart Thread/IsolateGroup/ClassTable/ObjectStore are native C++
// objects and may therefore carry a non-zero top byte even though their actual
// virtual address is in the lower 56 bits. Dart heap tagged pointers are not
// passed through this function.
uintptr_t CanonicalNativePointer(uint64_t pointer) {
#if defined(__aarch64__)
    return static_cast<uintptr_t>(pointer & 0x00ffffffffffffffULL);
#else
    return static_cast<uintptr_t>(pointer);
#endif
}

bool ReadNativePointer(const ProcessMemoryReader& reader, uintptr_t address,
                       uint64_t* out_pointer) {
    if (out_pointer == nullptr) return false;
    uint64_t raw = 0;
    if (!reader.Read(address, &raw) || raw == 0) return false;
    *out_pointer = CanonicalNativePointer(raw);
    return *out_pointer != 0;
}

uint64_t DecompressObject(uint64_t heap_base, uint32_t compressed) {
    return heap_base + static_cast<uint64_t>(compressed);
}

bool ReadCid(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
             uint64_t tagged, uint32_t* out_cid) {
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (out_cid == nullptr || raw == nullptr || raw->class_id_tag_bits == 0 ||
        raw->class_id_tag_bits >= 64 || !IsHeapObject(profile, tagged)) {
        return false;
    }
    uint64_t tags = 0;
    if (!reader.Read(Untag(profile, tagged), &tags)) return false;
    const uint64_t mask = (uint64_t{1} << raw->class_id_tag_bits) - 1;
    *out_cid = static_cast<uint32_t>((tags >> raw->class_id_tag_shift) & mask);
    return true;
}

bool RequireCid(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                uint64_t tagged, uint32_t expected) {
    uint32_t cid = 0;
    return ReadCid(reader, profile, tagged, &cid) && cid == expected;
}

bool ReadCidSafely(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                   uint64_t tagged, uint32_t* out_cid) {
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (out_cid == nullptr || raw == nullptr || raw->class_id_tag_bits == 0 ||
        raw->class_id_tag_bits >= 64 || !IsHeapObject(profile, tagged)) {
        return false;
    }
    uint64_t tags = 0;
    if (!reader.ReadSafely(Untag(profile, tagged), &tags)) return false;
    const uint64_t mask = (uint64_t{1} << raw->class_id_tag_bits) - 1;
    *out_cid = static_cast<uint32_t>((tags >> raw->class_id_tag_shift) & mask);
    return true;
}

bool ReadCompressedObject(const ProcessMemoryReader& reader, uintptr_t object_address,
                          const DartPlantLiveVmProfile& profile, uint32_t offset,
                          uint64_t heap_base, uint64_t* out_tagged) {
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (out_tagged == nullptr || raw == nullptr || raw->compressed_word_size != sizeof(uint32_t))
        return false;
    uint32_t compressed = 0;
    if (!reader.Read(object_address + offset, &compressed) ||
        (compressed & raw->smi_tag_mask) != raw->heap_object_tag) {
        return false;
    }
    *out_tagged = DecompressObject(heap_base, compressed);
    return true;
}

bool ReadPositiveCompressedSmi(const ProcessMemoryReader& reader, uintptr_t address,
                               const DartPlantLiveVmProfile& profile, uint64_t* out_value) {
    const RawObjectLayout* layout = FindRawObjectLayout(profile.profile_version);
    if (out_value == nullptr || layout == nullptr ||
        layout->compressed_word_size != sizeof(uint32_t))
        return false;
    uint32_t raw = 0;
    if (!reader.Read(address, &raw) || (raw & layout->smi_tag_mask) != layout->smi_tag)
        return false;
    *out_value = static_cast<uint64_t>(raw >> layout->smi_tag_shift);
    return true;
}

bool ReadDartString(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                    uint64_t tagged, char* output, size_t capacity) {
    if (output == nullptr || capacity == 0 || !IsHeapObject(profile, tagged)) return false;
    output[0] = '\0';
    uint32_t cid = 0;
    if (!ReadCid(reader, profile, tagged, &cid) ||
        (cid != profile.cid_one_byte_string && cid != profile.cid_two_byte_string)) {
        return false;
    }
    const uintptr_t object = Untag(profile, tagged);
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, object + profile.string_length_offset, profile,
                                   &length) ||
        length >= capacity) {
        return false;
    }
    if (cid == profile.cid_one_byte_string) {
        if (!reader.ReadBytes(object + profile.string_data_offset, output,
                              static_cast<size_t>(length))) {
            return false;
        }
        output[length] = '\0';
        return true;
    }

    if (length > (std::numeric_limits<size_t>::max() / sizeof(uint16_t))) return false;
    std::vector<uint16_t> units(static_cast<size_t>(length));
    if (!reader.ReadBytes(object + profile.string_data_offset, units.data(),
                          units.size() * sizeof(uint16_t))) {
        return false;
    }
    size_t cursor = 0;
    for (uint16_t unit : units) {
        if (unit <= 0x7f) {
            if (cursor + 1 >= capacity) return false;
            output[cursor++] = static_cast<char>(unit);
        } else if (unit <= 0x7ff) {
            if (cursor + 2 >= capacity) return false;
            output[cursor++] = static_cast<char>(0xc0 | (unit >> 6));
            output[cursor++] = static_cast<char>(0x80 | (unit & 0x3f));
        } else {
            if (cursor + 3 >= capacity) return false;
            output[cursor++] = static_cast<char>(0xe0 | (unit >> 12));
            output[cursor++] = static_cast<char>(0x80 | ((unit >> 6) & 0x3f));
            output[cursor++] = static_cast<char>(0x80 | (unit & 0x3f));
        }
    }
    output[cursor] = '\0';
    return true;
}

bool ReadArrayElement(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                      uint64_t heap_base, uint64_t tagged_array, uint64_t index,
                      uint64_t* out_tagged) {
    if (out_tagged == nullptr) return false;
    uint32_t cid = 0;
    if (!ReadCid(reader, profile, tagged_array, &cid) ||
        (cid != profile.cid_array && cid != profile.cid_immutable_array)) {
        return false;
    }
    const uintptr_t array = Untag(profile, tagged_array);
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, array + profile.array_length_offset, profile, &length) ||
        index >= length) {
        return false;
    }
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr || raw->compressed_word_size != sizeof(uint32_t)) return false;
    uint32_t compressed = 0;
    if (!reader.Read(array + profile.array_elements_offset + index * raw->compressed_word_size,
                     &compressed) ||
        (compressed & raw->smi_tag_mask) != raw->heap_object_tag) {
        return false;
    }
    *out_tagged = DecompressObject(heap_base, compressed);
    return true;
}

bool ReadArrayLength(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                     uint64_t tagged_array, uint64_t* out_length) {
    if (out_length == nullptr) return false;
    uint32_t cid = 0;
    return ReadCid(reader, profile, tagged_array, &cid) &&
           (cid == profile.cid_array || cid == profile.cid_immutable_array) &&
           ReadPositiveCompressedSmi(reader,
                                     Untag(profile, tagged_array) + profile.array_length_offset,
                                     profile, out_length);
}

bool ReadArrayRawElement(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                         uint64_t tagged_array, uint64_t index, uint32_t* out_raw) {
    if (out_raw == nullptr) return false;
    uint64_t length = 0;
    if (!ReadArrayLength(reader, profile, tagged_array, &length) || index >= length) return false;
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr || raw->compressed_word_size != sizeof(uint32_t)) return false;
    return reader.Read(Untag(profile, tagged_array) + profile.array_elements_offset +
                           index * raw->compressed_word_size,
                       out_raw);
}

bool ArrayContainsFunction(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                           uint64_t heap_base, uint64_t tagged_array, uint64_t tagged_function) {
    uint32_t cid = 0;
    if (!ReadCid(reader, profile, tagged_array, &cid) ||
        (cid != profile.cid_array && cid != profile.cid_immutable_array)) {
        return false;
    }
    const uintptr_t array = Untag(profile, tagged_array);
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, array + profile.array_length_offset, profile, &length) ||
        length > kMaxClassFunctions) {
        return false;
    }
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr || raw->compressed_word_size != sizeof(uint32_t)) return false;
    for (uint64_t index = 0; index < length; ++index) {
        uint32_t compressed = 0;
        const uintptr_t slot =
            array + profile.array_elements_offset + index * raw->compressed_word_size;
        if (!reader.Read(slot, &compressed)) return false;
        if ((compressed & raw->smi_tag_mask) != raw->heap_object_tag) continue;
        if (DecompressObject(heap_base, compressed) == tagged_function) return true;
    }
    return false;
}

bool IsClosureFunction(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                       uint64_t tagged_function) {
    uint32_t kind_tag = 0;
    if (!reader.Read(Untag(profile, tagged_function) + profile.function_kind_tag_offset,
                     &kind_tag)) {
        return true;
    }
    uint32_t kind = 0;
    return !DecodeFunctionKind(profile, kind_tag, &kind) ||
           IsClosureFunctionKind(profile.profile_version, kind);
}

bool FunctionNameMatches(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                         uint64_t heap_base, uint64_t tagged_function, const char* expected_name) {
    if (expected_name == nullptr || expected_name[0] == '\0') return true;
    uint64_t tagged_name = 0;
    char name[DARTPLANT_LIVE_VM_FUNCTION_NAME_MAX] = {};
    return ReadCompressedObject(reader, Untag(profile, tagged_function), profile,
                                profile.function_name_offset, heap_base, &tagged_name) &&
           ReadDartString(reader, profile, tagged_name, name, sizeof(name)) &&
           std::strcmp(name, expected_name) == 0;
}

bool ClassIdentityMatches(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                          uint64_t heap_base, uint64_t tagged_class, bool is_top_level,
                          const char* expected_class) {
    if (expected_class == nullptr || expected_class[0] == '\0') return true;
    if (std::strcmp(expected_class, "Global") == 0) return is_top_level;
    if (is_top_level) return false;

    uint64_t tagged_name = 0;
    char name[DARTPLANT_LIVE_VM_CLASS_NAME_MAX] = {};
    return ReadCompressedObject(reader, Untag(profile, tagged_class), profile,
                                profile.class_name_offset, heap_base, &tagged_name) &&
           ReadDartString(reader, profile, tagged_name, name, sizeof(name)) &&
           std::strcmp(name, expected_class) == 0;
}

bool ReadClassLibrary(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                      uint64_t heap_base, uint64_t tagged_class, uint64_t* out_library) {
    return out_library != nullptr && RequireCid(reader, profile, tagged_class, profile.cid_class) &&
           ReadCompressedObject(reader, Untag(profile, tagged_class), profile,
                                profile.class_library_offset, heap_base, out_library) &&
           RequireCid(reader, profile, *out_library, profile.cid_library);
}

bool ReadLibraryUri(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                    uint64_t heap_base, uint64_t tagged_library, char* output, size_t capacity) {
    if (!RequireCid(reader, profile, tagged_library, profile.cid_library)) return false;
    uint64_t tagged_url = 0;
    return ReadCompressedObject(reader, Untag(profile, tagged_library), profile,
                                profile.library_url_offset, heap_base, &tagged_url) &&
           ReadDartString(reader, profile, tagged_url, output, capacity);
}

bool LibraryIdentityMatches(const ProcessMemoryReader& reader,
                            const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                            uint64_t tagged_library, const char* expected_library) {
    if (expected_library == nullptr || expected_library[0] == '\0') return true;
    char library_uri[DARTPLANT_LIVE_VM_LIBRARY_URI_MAX] = {};
    return ReadLibraryUri(reader, profile, heap_base, tagged_library, library_uri,
                          sizeof(library_uri)) &&
           std::strcmp(library_uri, expected_library) == 0;
}

bool FindFunctionByIdentityInClass(const ProcessMemoryReader& reader,
                                   const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                                   uint64_t tagged_class, bool is_top_level,
                                   const char* expected_library, const char* expected_class,
                                   const char* expected_function, uint64_t* out_function,
                                   uint64_t* out_library) {
    if (out_function == nullptr || out_library == nullptr ||
        !ClassIdentityMatches(reader, profile, heap_base, tagged_class, is_top_level,
                              expected_class)) {
        return false;
    }
    uint64_t library = 0;
    if (!ReadClassLibrary(reader, profile, heap_base, tagged_class, &library) ||
        !LibraryIdentityMatches(reader, profile, heap_base, library, expected_library)) {
        return false;
    }
    uint64_t functions = 0;
    if (!ReadCompressedObject(reader, Untag(profile, tagged_class), profile,
                              profile.class_functions_offset, heap_base, &functions)) {
        return false;
    }
    uint32_t array_cid = 0;
    if (!ReadCid(reader, profile, functions, &array_cid) ||
        (array_cid != profile.cid_array && array_cid != profile.cid_immutable_array)) {
        return false;
    }
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, Untag(profile, functions) + profile.array_length_offset,
                                   profile, &length) ||
        length > kMaxClassFunctions) {
        return false;
    }
    for (uint64_t index = 0; index < length; ++index) {
        uint64_t function = 0;
        if (!ReadArrayElement(reader, profile, heap_base, functions, index, &function) ||
            !RequireCid(reader, profile, function, profile.cid_function)) {
            continue;
        }
        if (!IsClosureFunction(reader, profile, function) &&
            FunctionNameMatches(reader, profile, heap_base, function, expected_function)) {
            *out_function = function;
            *out_library = library;
            return true;
        }
    }
    return false;
}

bool FindFunctionByIdentity(const ProcessMemoryReader& reader,
                            const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                            uintptr_t class_table, uintptr_t cached_class_table_table,
                            uintptr_t object_store, const char* expected_library,
                            const char* expected_class, const char* expected_function,
                            uint64_t* out_function, uint64_t* out_library) {
    if (out_function == nullptr || out_library == nullptr) return false;
    *out_function = 0;
    *out_library = 0;

    uint64_t num_cids = 0;
    const uint64_t max_cids = MaxCidCount(profile);
    if (max_cids != 0 && class_table != 0 && cached_class_table_table != 0 &&
        reader.Read(class_table + profile.class_table_num_cids_offset, &num_cids) && num_cids > 0 &&
        num_cids <= max_cids) {
        for (uint64_t cid = 1; cid < num_cids; ++cid) {
            uint64_t tagged_class = 0;
            if (!reader.Read(cached_class_table_table + cid * sizeof(uint64_t), &tagged_class) ||
                tagged_class == 0) {
                continue;
            }
            if (FindFunctionByIdentityInClass(reader, profile, heap_base, tagged_class, false,
                                              expected_library, expected_class, expected_function,
                                              out_function, out_library)) {
                return true;
            }
        }
    }

    uint64_t libraries = 0;
    if (object_store == 0 ||
        !reader.Read(object_store + profile.object_store_libraries_offset, &libraries) ||
        !RequireCid(reader, profile, libraries, profile.cid_growable_object_array)) {
        return false;
    }
    const uintptr_t growable = Untag(profile, libraries);
    uint64_t length = 0;
    uint64_t data = 0;
    if (!ReadPositiveCompressedSmi(reader, growable + profile.growable_object_array_length_offset,
                                   profile, &length) ||
        length > kMaxClassFunctions ||
        !ReadCompressedObject(reader, growable, profile, profile.growable_object_array_data_offset,
                              heap_base, &data)) {
        return false;
    }
    for (uint64_t index = 0; index < length; ++index) {
        uint64_t library = 0;
        if (!ReadArrayElement(reader, profile, heap_base, data, index, &library) ||
            !RequireCid(reader, profile, library, profile.cid_library) ||
            !LibraryIdentityMatches(reader, profile, heap_base, library, expected_library)) {
            continue;
        }
        uint64_t top_level_class = 0;
        if (!ReadCompressedObject(reader, Untag(profile, library), profile,
                                  profile.library_toplevel_class_offset, heap_base,
                                  &top_level_class)) {
            continue;
        }
        if (FindFunctionByIdentityInClass(reader, profile, heap_base, top_level_class, true,
                                          expected_library, expected_class, expected_function,
                                          out_function, out_library)) {
            return true;
        }
    }
    return false;
}

bool ReadFunctionEntryForKind(const ProcessMemoryReader& reader,
                              const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                              uint64_t function, DartPlantEntryKind entry_kind,
                              uint64_t* out_entry) {
    if (out_entry == nullptr || !RequireCid(reader, profile, function, profile.cid_function))
        return false;
    const uintptr_t function_address = Untag(profile, function);
    if (entry_kind == DARTPLANT_ENTRY_DEFAULT) {
        return reader.Read(function_address + profile.function_entry_point_offset, out_entry) &&
               *out_entry != 0;
    }
    if (entry_kind == DARTPLANT_ENTRY_UNCHECKED) {
        return reader.Read(function_address + profile.function_unchecked_entry_point_offset,
                           out_entry) &&
               *out_entry != 0;
    }
    if (IsClosureFunction(reader, profile, function)) return false;
    uint64_t code = 0;
    if (!ReadCompressedObject(reader, function_address, profile, profile.function_code_offset,
                              heap_base, &code) ||
        !RequireCid(reader, profile, code, profile.cid_code)) {
        return false;
    }
    uint32_t offset = 0;
    if (entry_kind == DARTPLANT_ENTRY_MONOMORPHIC) {
        offset = profile.code_monomorphic_entry_point_offset;
    } else if (entry_kind == DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED) {
        offset = profile.code_monomorphic_unchecked_entry_point_offset;
    } else {
        return false;
    }
    return reader.Read(Untag(profile, code) + offset, out_entry) && *out_entry != 0;
}

bool ScanFunctionsByEntryInClass(const ProcessMemoryReader& reader,
                                 const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                                 uint64_t tagged_class, bool is_top_level, uintptr_t target_entry,
                                 DartPlantEntryKind entry_kind, const char* expected_function,
                                 const char* expected_class, uint64_t* selected_function,
                                 uint32_t* alias_count) {
    if (selected_function == nullptr || alias_count == nullptr ||
        !RequireCid(reader, profile, tagged_class, profile.cid_class)) {
        return false;
    }
    uint64_t functions = 0;
    if (!ReadCompressedObject(reader, Untag(profile, tagged_class), profile,
                              profile.class_functions_offset, heap_base, &functions)) {
        return false;
    }
    uint32_t array_cid = 0;
    if (!ReadCid(reader, profile, functions, &array_cid) ||
        (array_cid != profile.cid_array && array_cid != profile.cid_immutable_array)) {
        return false;
    }
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, Untag(profile, functions) + profile.array_length_offset,
                                   profile, &length) ||
        length > kMaxClassFunctions) {
        return false;
    }
    for (uint64_t index = 0; index < length; ++index) {
        uint64_t function = 0;
        if (!ReadArrayElement(reader, profile, heap_base, functions, index, &function) ||
            !RequireCid(reader, profile, function, profile.cid_function)) {
            continue;
        }
        uint64_t entry = 0;
        if (!ReadFunctionEntryForKind(reader, profile, heap_base, function, entry_kind, &entry) ||
            entry != target_entry) {
            continue;
        }
        if (*alias_count != std::numeric_limits<uint32_t>::max()) ++*alias_count;
        if (*selected_function == 0 &&
            FunctionNameMatches(reader, profile, heap_base, function, expected_function) &&
            ClassIdentityMatches(reader, profile, heap_base, tagged_class, is_top_level,
                                 expected_class)) {
            *selected_function = function;
        }
    }
    return true;
}

bool FindFunctionByEntryIdentity(const ProcessMemoryReader& reader,
                                 const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                                 uintptr_t class_table, uintptr_t cached_class_table_table,
                                 uintptr_t object_store, uintptr_t target_entry,
                                 DartPlantEntryKind entry_kind, const char* expected_function,
                                 const char* expected_class, uint64_t* out_function,
                                 uint32_t* out_alias_count) {
    if (out_function == nullptr || out_alias_count == nullptr || target_entry == 0) return false;
    *out_function = 0;
    *out_alias_count = 0;

    // Normal classes: cached_class_table_table is an atomic ClassPtr* published
    // specifically for fast readers. Its entries are full Dart tagged pointers.
    uint64_t num_cids = 0;
    const uint64_t max_cids = MaxCidCount(profile);
    if (max_cids != 0 && class_table != 0 && cached_class_table_table != 0 &&
        reader.Read(class_table + profile.class_table_num_cids_offset, &num_cids) && num_cids > 0 &&
        num_cids <= max_cids) {
        for (uint64_t cid = 1; cid < num_cids; ++cid) {
            uint64_t tagged_class = 0;
            if (!reader.Read(cached_class_table_table + cid * sizeof(uint64_t), &tagged_class) ||
                tagged_class == 0) {
                continue;
            }
            ScanFunctionsByEntryInClass(reader, profile, heap_base, tagged_class, false,
                                        target_entry, entry_kind, expected_function, expected_class,
                                        out_function, out_alias_count);
        }
    }

    // Top-level classes do not live in the normal cached class table. Walk the
    // ObjectStore's GrowableObjectArray of libraries, then each library's
    // toplevel_class. ObjectStore fields are full tagged Dart pointers.
    uint64_t libraries = 0;
    if (object_store == 0 ||
        !reader.Read(object_store + profile.object_store_libraries_offset, &libraries) ||
        !RequireCid(reader, profile, libraries, profile.cid_growable_object_array)) {
        return *out_function != 0;
    }
    const uintptr_t growable = Untag(profile, libraries);
    uint64_t length = 0;
    uint64_t data = 0;
    if (!ReadPositiveCompressedSmi(reader, growable + profile.growable_object_array_length_offset,
                                   profile, &length) ||
        length > kMaxClassFunctions ||
        !ReadCompressedObject(reader, growable, profile, profile.growable_object_array_data_offset,
                              heap_base, &data)) {
        return *out_function != 0;
    }
    for (uint64_t index = 0; index < length; ++index) {
        uint64_t library = 0;
        if (!ReadArrayElement(reader, profile, heap_base, data, index, &library) ||
            !RequireCid(reader, profile, library, profile.cid_library)) {
            continue;
        }
        uint64_t top_level_class = 0;
        if (!ReadCompressedObject(reader, Untag(profile, library), profile,
                                  profile.library_toplevel_class_offset, heap_base,
                                  &top_level_class)) {
            continue;
        }
        ScanFunctionsByEntryInClass(reader, profile, heap_base, top_level_class, true, target_entry,
                                    entry_kind, expected_function, expected_class, out_function,
                                    out_alias_count);
    }
    return *out_function != 0;
}

struct CollectedLiveFunction {
    DartPlantLiveVmFunctionInfo info{};
};

uint64_t EntryForKind(const DartPlantLiveVmFunctionInfo& info, DartPlantEntryKind kind) {
    switch (kind) {
    case DARTPLANT_ENTRY_DEFAULT:
        return info.code_entry_point;
    case DARTPLANT_ENTRY_UNCHECKED:
        return info.code_unchecked_entry_point;
    case DARTPLANT_ENTRY_MONOMORPHIC:
        return info.code_monomorphic_entry_point;
    case DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED:
        return info.code_monomorphic_unchecked_entry_point;
    }
    return 0;
}

bool RuntimeInstructionEntryToVa(const DartPlantFlutterSnapshotInfo& snapshot, uint64_t entry,
                                 uint64_t* out_va) {
    if (out_va == nullptr || snapshot.isolate_instructions_size == 0 ||
        entry < snapshot.isolate_instructions_runtime) {
        return false;
    }
    const uint64_t offset = entry - snapshot.isolate_instructions_runtime;
    if (offset >= snapshot.isolate_instructions_size ||
        snapshot.isolate_instructions_va > std::numeric_limits<uint64_t>::max() - offset) {
        return false;
    }
    *out_va = snapshot.isolate_instructions_va + offset;
    return true;
}

bool CollectLiveFunction(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                         uint64_t heap_base, uint64_t tagged_function, uint32_t kind,
                         uint64_t tagged_class, uint64_t library, bool is_top_level,
                         const char* library_uri, const char* class_name,
                         const DartPlantFlutterSnapshotInfo& snapshot,
                         std::vector<CollectedLiveFunction>* functions) {
    if (functions == nullptr || library_uri == nullptr || class_name == nullptr ||
        !RequireCid(reader, profile, tagged_function, profile.cid_function)) {
        return false;
    }

    const uintptr_t function_address = Untag(profile, tagged_function);
    CollectedLiveFunction collected{};
    collected.info.struct_size = sizeof(collected.info);
    collected.info.function = tagged_function;
    collected.info.owner_class = tagged_class;
    collected.info.library = library;
    collected.info.owner_is_toplevel_class = is_top_level ? 1 : 0;
    collected.info.function_kind = kind;
    std::snprintf(collected.info.library_uri, sizeof(collected.info.library_uri), "%s",
                  library_uri);
    std::snprintf(collected.info.class_name, sizeof(collected.info.class_name), "%s", class_name);

    uint64_t tagged_name = 0;
    uint64_t function_owner = 0;
    if (!reader.Read(function_address + profile.function_entry_point_offset,
                     &collected.info.function_entry_point) ||
        !reader.Read(function_address + profile.function_unchecked_entry_point_offset,
                     &collected.info.function_unchecked_entry_point) ||
        !ReadCompressedObject(reader, function_address, profile, profile.function_name_offset,
                              heap_base, &tagged_name) ||
        !ReadDartString(reader, profile, tagged_name, collected.info.function_name,
                        sizeof(collected.info.function_name)) ||
        !ReadCompressedObject(reader, function_address, profile, profile.function_owner_offset,
                              heap_base, &function_owner) ||
        function_owner != tagged_class ||
        !ReadCompressedObject(reader, function_address, profile, profile.function_code_offset,
                              heap_base, &collected.info.code) ||
        !RequireCid(reader, profile, collected.info.code, profile.cid_code)) {
        return false;
    }

    const bool closure_call_entry_only = IsClosureFunctionKind(profile.profile_version, kind);
    uint64_t code_owner = 0;
    const uintptr_t code_address = Untag(profile, collected.info.code);
    if (!reader.Read(code_address + profile.code_entry_point_offset,
                     &collected.info.code_entry_point) ||
        !reader.Read(code_address + profile.code_unchecked_entry_point_offset,
                     &collected.info.code_unchecked_entry_point) ||
        !reader.Read(code_address + profile.code_monomorphic_entry_point_offset,
                     &collected.info.code_monomorphic_entry_point) ||
        !reader.Read(code_address + profile.code_monomorphic_unchecked_entry_point_offset,
                     &collected.info.code_monomorphic_unchecked_entry_point) ||
        !reader.Read(code_address + profile.code_object_pool_offset,
                     &collected.info.code_object_pool) ||
        !reader.Read(code_address + profile.code_owner_offset, &code_owner) ||
        !reader.Read(code_address + profile.code_instructions_length_offset,
                     &collected.info.code_size) ||
        collected.info.function_entry_point == 0 || collected.info.code_entry_point == 0 ||
        collected.info.code_size == 0) {
        return false;
    }
    // AOT closure invocation loads Closure.entry_point and calls only the
    // Function normal entry. The remaining Function/Code caches are not part
    // of that call contract and may legitimately be absent or aliased. Regular
    // Functions expose all four CodeEntryKind caches and must agree exactly.
    if (collected.info.function_entry_point != collected.info.code_entry_point ||
        (!closure_call_entry_only && (collected.info.function_unchecked_entry_point == 0 ||
                                      collected.info.code_unchecked_entry_point == 0 ||
                                      collected.info.code_monomorphic_entry_point == 0 ||
                                      collected.info.code_monomorphic_unchecked_entry_point == 0 ||
                                      collected.info.function_unchecked_entry_point !=
                                          collected.info.code_unchecked_entry_point))) {
        return false;
    }
    collected.info.code_owner_matches_function = code_owner == tagged_function ? 1 : 0;
    if (!collected.info.code_owner_matches_function &&
        (!RequireCid(reader, profile, code_owner, profile.cid_function) ||
         !HasSnapshotFeature(snapshot.snapshot_features, "dedup_instructions"))) {
        return false;
    }

    AotCodePayloadRange payload_range{};
    if (!ComputeAotCodePayloadRange(profile.profile_version, collected.info.code_entry_point,
                                    collected.info.code_monomorphic_entry_point,
                                    collected.info.code_size, &payload_range)) {
        return false;
    }
    const uint64_t payload_start = payload_range.start;
    const uint64_t payload_end = payload_range.end;
    if (collected.info.code_unchecked_entry_point != 0 &&
        (collected.info.code_unchecked_entry_point < payload_start ||
         collected.info.code_unchecked_entry_point >= payload_end)) {
        return false;
    }
    if (collected.info.code_monomorphic_unchecked_entry_point != 0 &&
        (collected.info.code_monomorphic_unchecked_entry_point < payload_start ||
         collected.info.code_monomorphic_unchecked_entry_point >= payload_end)) {
        return false;
    }
    if (collected.info.code_unchecked_entry_point != 0 &&
        collected.info.code_monomorphic_unchecked_entry_point != 0) {
        if (collected.info.code_unchecked_entry_point < collected.info.code_entry_point ||
            collected.info.code_monomorphic_unchecked_entry_point <
                collected.info.code_monomorphic_entry_point) {
            return false;
        }
        const uint64_t unchecked_delta =
            collected.info.code_unchecked_entry_point - collected.info.code_entry_point;
        if (collected.info.code_monomorphic_unchecked_entry_point -
                collected.info.code_monomorphic_entry_point !=
            unchecked_delta) {
            return false;
        }
    }
    if (!RuntimeInstructionEntryToVa(snapshot, collected.info.function_entry_point,
                                     &collected.info.entry_va)) {
        return false;
    }
    if (!closure_call_entry_only &&
        (!RuntimeInstructionEntryToVa(snapshot, collected.info.function_unchecked_entry_point,
                                      &collected.info.unchecked_entry_va) ||
         !RuntimeInstructionEntryToVa(snapshot, collected.info.code_monomorphic_entry_point,
                                      &collected.info.monomorphic_entry_va) ||
         !RuntimeInstructionEntryToVa(snapshot,
                                      collected.info.code_monomorphic_unchecked_entry_point,
                                      &collected.info.monomorphic_unchecked_entry_va))) {
        return false;
    }
    collected.info.closure_call_entry_only = closure_call_entry_only ? 1 : 0;
    collected.info.entry_kind_mask = closure_call_entry_only ? 0x1u : 0x0fu;
    collected.info.code_section_va = snapshot.isolate_instructions_va;
    functions->push_back(collected);
    return true;
}

bool CollectFunctionsInClass(const ProcessMemoryReader& reader,
                             const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                             uint64_t tagged_class, bool is_top_level,
                             const DartPlantFlutterSnapshotInfo& snapshot,
                             std::unordered_set<uint64_t>* seen_functions,
                             std::vector<CollectedLiveFunction>* functions,
                             uint32_t* skipped_function_count) {
    if (seen_functions == nullptr || functions == nullptr || skipped_function_count == nullptr ||
        !RequireCid(reader, profile, tagged_class, profile.cid_class)) {
        return false;
    }

    uint64_t library = 0;
    uint64_t class_functions = 0;
    if (!ReadClassLibrary(reader, profile, heap_base, tagged_class, &library) ||
        !ReadCompressedObject(reader, Untag(profile, tagged_class), profile,
                              profile.class_functions_offset, heap_base, &class_functions)) {
        return false;
    }

    char library_uri[DARTPLANT_LIVE_VM_LIBRARY_URI_MAX] = {};
    char class_name[DARTPLANT_LIVE_VM_CLASS_NAME_MAX] = {};
    if (!ReadLibraryUri(reader, profile, heap_base, library, library_uri, sizeof(library_uri))) {
        return false;
    }
    if (is_top_level) {
        std::snprintf(class_name, sizeof(class_name), "%s", "Global");
    } else {
        uint64_t class_name_object = 0;
        if (!ReadCompressedObject(reader, Untag(profile, tagged_class), profile,
                                  profile.class_name_offset, heap_base, &class_name_object) ||
            !ReadDartString(reader, profile, class_name_object, class_name, sizeof(class_name))) {
            return false;
        }
    }

    uint32_t functions_cid = 0;
    if (!ReadCid(reader, profile, class_functions, &functions_cid) ||
        (functions_cid != profile.cid_array && functions_cid != profile.cid_immutable_array)) {
        return false;
    }
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader,
                                   Untag(profile, class_functions) + profile.array_length_offset,
                                   profile, &length) ||
        length > kMaxClassFunctions) {
        return false;
    }

    for (uint64_t index = 0; index < length; ++index) {
        uint64_t function = 0;
        if (!ReadArrayElement(reader, profile, heap_base, class_functions, index, &function) ||
            !RequireCid(reader, profile, function, profile.cid_function)) {
            ++*skipped_function_count;
            continue;
        }
        if (!seen_functions->insert(function).second) continue;

        const uintptr_t function_address = Untag(profile, function);
        uint32_t kind_tag = 0;
        if (!reader.Read(function_address + profile.function_kind_tag_offset, &kind_tag)) {
            ++*skipped_function_count;
            continue;
        }
        uint32_t kind = 0;
        if (!DecodeFunctionKind(profile, kind_tag, &kind)) {
            ++*skipped_function_count;
            continue;
        }
        if (!CollectLiveFunction(reader, profile, heap_base, function, kind, tagged_class, library,
                                 is_top_level, library_uri, class_name, snapshot, functions)) {
            ++*skipped_function_count;
        }
    }
    return true;
}

bool CollectAllLiveFunctions(const ProcessMemoryReader& reader,
                             const DartPlantLiveVmProfile& profile,
                             const DartPlantLiveVmContext& context,
                             const DartPlantFlutterSnapshotInfo& snapshot,
                             std::vector<CollectedLiveFunction>* functions,
                             DartPlantLiveVmFunctionIndexInfo* out_info) {
    if (functions == nullptr || out_info == nullptr) return false;
    functions->clear();
    uint32_t skipped = 0;
    std::unordered_set<uint64_t> seen_functions;

    const uint64_t max_cids = MaxCidCount(profile);
    uint64_t num_cids = 0;
    if (max_cids == 0 ||
        !reader.Read(
            static_cast<uintptr_t>(context.class_table) + profile.class_table_num_cids_offset,
            &num_cids) ||
        num_cids == 0 || num_cids > max_cids) {
        return false;
    }
    for (uint64_t cid = 1; cid < num_cids; ++cid) {
        uint64_t tagged_class = 0;
        if (!reader.Read(
                static_cast<uintptr_t>(context.cached_class_table_table) + cid * sizeof(uint64_t),
                &tagged_class) ||
            tagged_class == 0) {
            continue;
        }
        (void) CollectFunctionsInClass(reader, profile, context.heap_base, tagged_class, false,
                                       snapshot, &seen_functions, functions, &skipped);
    }

    uint64_t libraries = 0;
    if (!reader.Read(
            static_cast<uintptr_t>(context.object_store) + profile.object_store_libraries_offset,
            &libraries) ||
        !RequireCid(reader, profile, libraries, profile.cid_growable_object_array)) {
        return false;
    }
    const uintptr_t growable = Untag(profile, libraries);
    uint64_t library_count = 0;
    uint64_t data = 0;
    if (!ReadPositiveCompressedSmi(reader, growable + profile.growable_object_array_length_offset,
                                   profile, &library_count) ||
        library_count > kMaxClassFunctions ||
        !ReadCompressedObject(reader, growable, profile, profile.growable_object_array_data_offset,
                              context.heap_base, &data)) {
        return false;
    }
    for (uint64_t index = 0; index < library_count; ++index) {
        uint64_t library = 0;
        if (!ReadArrayElement(reader, profile, context.heap_base, data, index, &library) ||
            !RequireCid(reader, profile, library, profile.cid_library)) {
            continue;
        }
        uint64_t top_level_class = 0;
        if (!ReadCompressedObject(reader, Untag(profile, library), profile,
                                  profile.library_toplevel_class_offset, context.heap_base,
                                  &top_level_class)) {
            continue;
        }
        (void) CollectFunctionsInClass(reader, profile, context.heap_base, top_level_class, true,
                                       snapshot, &seen_functions, functions, &skipped);
    }

    std::array<std::unordered_map<uint64_t, uint32_t>, 4> aliases_by_kind;
    for (const CollectedLiveFunction& function : *functions) {
        for (uint32_t kind = 0; kind < 4; ++kind) {
            if ((function.info.entry_kind_mask & (1u << kind)) == 0) continue;
            const uint64_t entry =
                EntryForKind(function.info, static_cast<DartPlantEntryKind>(kind));
            uint32_t& count = aliases_by_kind[kind][entry];
            if (count != std::numeric_limits<uint32_t>::max()) ++count;
        }
    }
    for (auto& function : *functions) {
        for (uint32_t kind = 0; kind < 4; ++kind) {
            if ((function.info.entry_kind_mask & (1u << kind)) == 0) continue;
            const uint64_t entry =
                EntryForKind(function.info, static_cast<DartPlantEntryKind>(kind));
            function.info.entry_alias_counts[kind] = aliases_by_kind[kind][entry];
        }
        function.info.entry_alias_count = function.info.entry_alias_counts[DARTPLANT_ENTRY_DEFAULT];
        function.info.entry_is_shared = function.info.entry_alias_count > 1 ? 1 : 0;
    }
    // Keep the public aggregate index counters compatible with the original
    // Function-index contract: they describe default Function entries. Exact
    // per-entry-kind multiplicity is exposed on each FunctionInfo above.
    uint32_t shared_targets = 0;
    const auto& default_aliases = aliases_by_kind[DARTPLANT_ENTRY_DEFAULT];
    for (const auto& [entry, count] : default_aliases) {
        (void) entry;
        if (count > 1) ++shared_targets;
    }

    out_info->function_count = static_cast<uint32_t>(functions->size());
    out_info->code_target_count = static_cast<uint32_t>(default_aliases.size());
    out_info->shared_code_target_count = shared_targets;
    out_info->skipped_function_count = skipped;
    return true;
}

DartPlantStatus FailProbe(const char* message) {
    SetLastError(message);
    return DARTPLANT_PROFILE_MISMATCH;
}

DartPlantStatus SelectProfile(const DartPlantFlutterSnapshotInfo& snapshot,
                              DartPlantLiveVmProfile* out_profile) {
    if (snapshot.snapshot_hash == nullptr || snapshot.profile_name == nullptr) {
        return FailProbe("live VM snapshot identity is incomplete");
    }
    if (!snapshot.compressed_pointers) {
        return FailProbe("no exact live VM raw-layout profile for uncompressed pointers");
    }
    const RuntimeProfileRecord* matched =
        FindRuntimeProfileBySnapshot(snapshot.snapshot_hash, snapshot.profile_name);
    if (matched == nullptr) {
        return FailProbe("no exact live VM raw-layout profile for this Dart snapshot");
    }
    *out_profile = matched->live_vm;
    ClearLastError();
    return DARTPLANT_OK;
}

struct ParsedFunctionSignature {
    uint64_t signature = 0;
    uint64_t parameter_types = 0;
    uint64_t named_parameter_names = 0;
    uint32_t parameter_count = 0;
    uint32_t implicit_parameter_count = 0;
    uint32_t fixed_parameter_count = 0;
    uint32_t optional_parameter_count = 0;
    uint32_t type_parameter_count = 0;
    uint32_t parent_type_argument_count = 0;
    bool has_named_optional_parameters = false;
    DartPlantDartTypeInfo result_type{};
};

bool DecodeDartNullability(uint32_t flags, const FunctionTypeLayout& layout,
                           DartPlantDartNullability* out_nullability) {
    if (out_nullability == nullptr) return false;
    const uint32_t encoded = flags & ((1U << layout.nullability_bits) - 1U);
    if (layout.nullability_bits == 2) {
        switch (encoded) {
        case 0:
            *out_nullability = DARTPLANT_DART_NULLABILITY_NULLABLE;
            return true;
        case 1:
            *out_nullability = DARTPLANT_DART_NULLABILITY_NON_NULLABLE;
            return true;
        case 2:
            *out_nullability = DARTPLANT_DART_NULLABILITY_LEGACY;
            return true;
        default:
            return false;
        }
    }
    if (layout.nullability_bits == 1) {
        *out_nullability = encoded == 0 ? DARTPLANT_DART_NULLABILITY_NULLABLE
                                        : DARTPLANT_DART_NULLABILITY_NON_NULLABLE;
        return true;
    }
    return false;
}

bool DecodeDartType(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                    const FunctionTypeLayout& layout, uint64_t tagged_type,
                    DartPlantDartTypeInfo* out_type) {
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (out_type == nullptr || raw == nullptr || raw->class_id_tag_bits == 0 ||
        raw->class_id_tag_bits >= 32 || !IsHeapObject(profile, tagged_type)) {
        return false;
    }
    DartPlantDartTypeInfo type{};
    type.struct_size = sizeof(type);
    if (!ReadCid(reader, profile, tagged_type, &type.object_cid)) return false;
    if (type.object_cid != layout.cid_type && type.object_cid != layout.cid_function_type &&
        type.object_cid != layout.cid_record_type && type.object_cid != layout.cid_type_parameter) {
        return false;
    }

    uint32_t flags = 0;
    if (!reader.Read(Untag(profile, tagged_type) + layout.abstract_type_flags_offset, &flags) ||
        !DecodeDartNullability(flags, layout, &type.nullability)) {
        return false;
    }

    if (type.object_cid == layout.cid_type) {
        const uint32_t class_id_mask = (uint32_t{1} << raw->class_id_tag_bits) - 1;
        const uint32_t represented_cid =
            static_cast<uint32_t>((flags >> layout.type_class_id_shift) & class_id_mask);
        if (represented_cid == 0) return false;
        if (represented_cid == layout.cid_null) {
            type.kind = DARTPLANT_DART_TYPE_NULL;
        } else if (represented_cid == layout.cid_dynamic) {
            type.kind = DARTPLANT_DART_TYPE_DYNAMIC;
        } else if (represented_cid == layout.cid_void) {
            type.kind = DARTPLANT_DART_TYPE_VOID;
        } else if (represented_cid == layout.cid_never) {
            type.kind = DARTPLANT_DART_TYPE_NEVER;
        } else {
            type.kind = DARTPLANT_DART_TYPE_INTERFACE;
            type.type_class_id = represented_cid;
        }
    } else if (type.object_cid == layout.cid_function_type) {
        type.kind = DARTPLANT_DART_TYPE_FUNCTION;
    } else if (type.object_cid == layout.cid_record_type) {
        type.kind = DARTPLANT_DART_TYPE_RECORD;
    } else {
        type.kind = DARTPLANT_DART_TYPE_PARAMETER;
        uint16_t base = 0;
        uint16_t index = 0;
        if (!reader.Read(Untag(profile, tagged_type) + layout.type_parameter_base_offset, &base) ||
            !reader.Read(Untag(profile, tagged_type) + layout.type_parameter_index_offset,
                         &index)) {
            return false;
        }
        type.type_parameter_base = base;
        type.type_parameter_index = index;
        type.is_function_type_parameter =
            static_cast<uint8_t>((flags >> layout.type_parameter_function_bit) & 0x1U);
    }
    *out_type = type;
    return true;
}

DartPlantStatus ParseRetainedFunctionSignature(const ProcessMemoryReader& reader,
                                               const DartPlantLiveVmProfile& profile,
                                               const FunctionTypeLayout& layout, uint64_t heap_base,
                                               uint64_t tagged_function,
                                               ParsedFunctionSignature* out_signature) {
    if (out_signature == nullptr || heap_base == 0 ||
        !RequireCid(reader, profile, tagged_function, profile.cid_function)) {
        return FailProbe("live VM Function is stale or has an invalid CID for signature parsing");
    }

    uint64_t tagged_signature = 0;
    if (!ReadCompressedObject(reader, Untag(profile, tagged_function), profile,
                              layout.function_signature_offset, heap_base, &tagged_signature)) {
        return FailProbe("live VM Function.signature is unreadable");
    }
    uint32_t signature_cid = 0;
    if (!ReadCid(reader, profile, tagged_signature, &signature_cid)) {
        return FailProbe("live VM Function.signature is not a readable heap object");
    }
    if (signature_cid == layout.cid_null) {
        SetLastError("AOT precompiler dropped Function.signature for this function");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (signature_cid != layout.cid_function_type) {
        return FailProbe("live VM Function.signature has an unexpected CID");
    }

    const uintptr_t signature_address = Untag(profile, tagged_signature);
    uint32_t packed_counts = 0;
    uint16_t packed_type_counts = 0;
    uint64_t result_type = 0;
    if (!reader.Read(signature_address + layout.packed_parameter_counts_offset, &packed_counts) ||
        !reader.Read(signature_address + layout.packed_type_parameter_counts_offset,
                     &packed_type_counts) ||
        !ReadCompressedObject(reader, signature_address, profile, layout.result_type_offset,
                              heap_base, &result_type)) {
        return FailProbe("live VM FunctionType fields are unreadable");
    }

    ParsedFunctionSignature parsed{};
    parsed.signature = tagged_signature;
    parsed.implicit_parameter_count = packed_counts & 0x1U;
    parsed.has_named_optional_parameters = ((packed_counts >> 1) & 0x1U) != 0;
    parsed.fixed_parameter_count = (packed_counts >> 2) & 0x3fffU;
    parsed.optional_parameter_count = (packed_counts >> 16) & 0x3fffU;
    parsed.parameter_count = parsed.fixed_parameter_count + parsed.optional_parameter_count;
    parsed.parent_type_argument_count = packed_type_counts & 0xffU;
    parsed.type_parameter_count = (packed_type_counts >> 8) & 0xffU;
    if (parsed.implicit_parameter_count > parsed.fixed_parameter_count ||
        (parsed.has_named_optional_parameters && parsed.optional_parameter_count == 0)) {
        return FailProbe("live VM FunctionType parameter counts are inconsistent");
    }
    if (!DecodeDartType(reader, profile, layout, result_type, &parsed.result_type)) {
        return FailProbe("live VM FunctionType result type is invalid");
    }

    if (parsed.parameter_count != 0) {
        if (!ReadCompressedObject(reader, signature_address, profile, layout.parameter_types_offset,
                                  heap_base, &parsed.parameter_types)) {
            return FailProbe("live VM FunctionType parameter_types is unreadable");
        }
        uint64_t parameter_type_count = 0;
        if (!ReadArrayLength(reader, profile, parsed.parameter_types, &parameter_type_count) ||
            parameter_type_count != parsed.parameter_count) {
            return FailProbe("live VM FunctionType parameter_types length does not match counts");
        }
    }

    if (parsed.has_named_optional_parameters) {
        if (!ReadCompressedObject(reader, signature_address, profile,
                                  layout.named_parameter_names_offset, heap_base,
                                  &parsed.named_parameter_names)) {
            return FailProbe("live VM FunctionType named_parameter_names is unreadable");
        }
        uint64_t named_slot_count = 0;
        const uint64_t flag_slot_count =
            (parsed.optional_parameter_count + kNamedParameterFlagsPerSmi - 1) /
            kNamedParameterFlagsPerSmi;
        const uint64_t expected_named_slot_count =
            static_cast<uint64_t>(parsed.optional_parameter_count) + flag_slot_count;
        if (!ReadArrayLength(reader, profile, parsed.named_parameter_names, &named_slot_count) ||
            named_slot_count != expected_named_slot_count) {
            return FailProbe("live VM FunctionType named_parameter_names is inconsistent");
        }
    }

    *out_signature = parsed;
    return DARTPLANT_OK;
}

DartPlantStatus PrepareFunctionSignatureRead(const DartPlantLiveVmContext& context,
                                             const DartPlantFlutterSnapshotInfo& snapshot,
                                             uint64_t tagged_function,
                                             DartPlantLiveVmProfile* out_profile,
                                             const FunctionTypeLayout** out_layout,
                                             ProcessMemoryReader* out_reader) {
    if (out_profile == nullptr || out_layout == nullptr || out_reader == nullptr ||
        tagged_function == 0) {
        SetLastError("live VM signature parser arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    out_profile->struct_size = sizeof(*out_profile);
    DartPlantStatus status = SelectProfile(snapshot, out_profile);
    if (status != DARTPLANT_OK) return status;
    if (context.profile_version != out_profile->profile_version ||
        !SameString(context.profile_name, out_profile->name) || context.heap_base == 0) {
        SetLastError("live VM context profile does not match FunctionType parser profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    *out_layout = FindFunctionTypeLayout(out_profile->profile_version);
    if (*out_layout == nullptr) {
        SetLastError("no FunctionType layout for selected live VM profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (!out_reader->Refresh()) {
        SetLastError("cannot inspect process mappings for FunctionType parsing");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    return DARTPLANT_OK;
}

}  // namespace

DartPlantStatus ResolveLiveVmCanonicalBoolRoots(const DartPlantLiveVmContext& context,
                                                const DartPlantLiveVmProfile& profile,
                                                uint64_t* out_true, uint64_t* out_false) {
    const CanonicalBoolLayout* layout = FindCanonicalBoolLayout(profile.profile_version);
    if (out_true == nullptr || out_false == nullptr || context.thread == 0 || layout == nullptr) {
        SetLastError("live VM Bool root arguments/profile are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (context.profile_version != profile.profile_version) {
        SetLastError("live VM Bool root profile does not match the captured context");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    ProcessMemoryReader reader;
    if (!reader.Refresh()) {
        SetLastError("failed to inspect process mappings for Dart Bool roots");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    uint64_t bool_true = 0;
    uint64_t bool_false = 0;
    if (!reader.Read(static_cast<uintptr_t>(context.thread) + layout->thread_true_offset,
                     &bool_true) ||
        !reader.Read(static_cast<uintptr_t>(context.thread) + layout->thread_false_offset,
                     &bool_false) ||
        bool_true == 0 || bool_false == 0 || bool_true == bool_false ||
        !RequireCid(reader, profile, bool_true, layout->cid) ||
        !RequireCid(reader, profile, bool_false, layout->cid)) {
        SetLastError("Dart canonical Bool roots failed CID validation");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    uint8_t true_value = 0;
    uint8_t false_value = 0xff;
    if (!reader.Read(Untag(profile, bool_true) + layout->value_offset, &true_value) ||
        !reader.Read(Untag(profile, bool_false) + layout->value_offset, &false_value) ||
        true_value != 1 || false_value != 0) {
        SetLastError("Dart canonical Bool roots failed value validation");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    *out_true = bool_true;
    *out_false = bool_false;
    ClearLastError();
    return DARTPLANT_OK;
}

}  // namespace dartplant

extern "C" DartPlantStatus dartplant_live_vm_select_profile(
    const DartPlantFlutterSnapshotInfo* snapshot, DartPlantLiveVmProfile* out_profile) {
    if (snapshot == nullptr || out_profile == nullptr ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
        out_profile->struct_size < dartplant::kLiveVmProfileV1Size) {
        dartplant::SetLastError("live VM profile selection arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    DartPlantLiveVmProfile selected{};
    selected.struct_size = sizeof(selected);
    const DartPlantStatus status = dartplant::SelectProfile(*snapshot, &selected);
    if (status != DARTPLANT_OK) return status;
    dartplant::CopyOutputPrefix(selected, out_profile);
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_context_from_arm64_registers(
    const DartPlantFlutterSnapshotInfo* snapshot, const DartPlantLiveVmArm64Registers* registers,
    DartPlantLiveVmContext* out_context) {
    if (snapshot == nullptr || registers == nullptr || out_context == nullptr ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
        registers->struct_size < sizeof(DartPlantLiveVmArm64Registers) ||
        out_context->struct_size < sizeof(DartPlantLiveVmContext)) {
        dartplant::SetLastError("live VM register context arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    DartPlantStatus status = dartplant::SelectProfile(*snapshot, &profile);
    if (status != DARTPLANT_OK) return status;
    const dartplant::RawObjectLayout* raw = dartplant::FindRawObjectLayout(profile.profile_version);
    const uint64_t max_cids = dartplant::MaxCidCount(profile);
    if (raw == nullptr || max_cids == 0) {
        return dartplant::FailProbe("selected live VM raw-object profile is incomplete");
    }

    dartplant::ProcessMemoryReader reader;
    if (!reader.Refresh()) {
        return dartplant::FailProbe("cannot read /proc/self/maps for live VM register validation");
    }

    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    context.profile_version = profile.profile_version;
    context.profile_name = profile.name;
    context.thread = dartplant::CanonicalNativePointer(registers->thr);
    context.pp = registers->pp;
    if (context.thread == 0 ||
        !reader.Contains(static_cast<uintptr_t>(context.thread),
                         profile.thread_isolate_group_offset + sizeof(uint64_t))) {
        return dartplant::FailProbe("sampled THR is not a readable Dart Thread");
    }

    uint64_t thread_null = 0;
    uint64_t thread_pool = 0;
    if (!reader.Read(static_cast<uintptr_t>(context.thread) + profile.thread_heap_base_offset,
                     &context.heap_base) ||
        !reader.Read(static_cast<uintptr_t>(context.thread) + profile.thread_object_null_offset,
                     &thread_null) ||
        !reader.Read(
            static_cast<uintptr_t>(context.thread) + profile.thread_global_object_pool_offset,
            &thread_pool) ||
        !dartplant::ReadNativePointer(
            reader, static_cast<uintptr_t>(context.thread) + profile.thread_isolate_offset,
            &context.isolate) ||
        !dartplant::ReadNativePointer(
            reader, static_cast<uintptr_t>(context.thread) + profile.thread_isolate_group_offset,
            &context.isolate_group)) {
        return dartplant::FailProbe("sampled Dart Thread fields are unreadable");
    }

    if (static_cast<uint32_t>(registers->heap_bits) !=
            static_cast<uint32_t>(context.heap_base >> 32) ||
        registers->null_value != thread_null || !dartplant::IsHeapObject(profile, thread_pool) ||
        thread_pool < raw->heap_object_tag || context.pp != thread_pool - raw->heap_object_tag) {
        return dartplant::FailProbe("sampled THR/PP/HEAP_BITS/NULL_REG semantics do not match");
    }
    context.global_object_pool = thread_pool;

    if (context.isolate == 0 || context.isolate_group == 0 ||
        !reader.Contains(static_cast<uintptr_t>(context.isolate), sizeof(uint64_t)) ||
        !reader.Contains(static_cast<uintptr_t>(context.isolate_group),
                         profile.isolate_group_object_store_offset + sizeof(uint64_t)) ||
        !dartplant::ReadNativePointer(reader,
                                      static_cast<uintptr_t>(context.isolate_group) +
                                          profile.isolate_group_class_table_offset,
                                      &context.class_table) ||
        !dartplant::ReadNativePointer(reader,
                                      static_cast<uintptr_t>(context.isolate_group) +
                                          profile.isolate_group_cached_class_table_table_offset,
                                      &context.cached_class_table_table) ||
        !dartplant::ReadNativePointer(reader,
                                      static_cast<uintptr_t>(context.isolate_group) +
                                          profile.isolate_group_object_store_offset,
                                      &context.object_store)) {
        return dartplant::FailProbe("sampled Isolate/IsolateGroup roots are invalid");
    }

    if (!dartplant::RequireCid(reader, profile, context.global_object_pool,
                               profile.cid_object_pool) ||
        !reader.Read(dartplant::Untag(profile, context.global_object_pool) +
                         profile.object_pool_length_offset,
                     &context.object_pool_length) ||
        context.object_pool_length == 0 ||
        context.object_pool_length > dartplant::kMaxObjectPoolEntries) {
        return dartplant::FailProbe("sampled global ObjectPool is invalid");
    }

    uint64_t num_cids = 0;
    if (!reader.Read(
            static_cast<uintptr_t>(context.class_table) + profile.class_table_num_cids_offset,
            &num_cids) ||
        num_cids == 0 || num_cids > max_cids) {
        return dartplant::FailProbe("sampled ClassTable is invalid");
    }
    uint64_t class_class = 0;
    if (profile.cid_class >= num_cids ||
        !reader.Read(static_cast<uintptr_t>(context.cached_class_table_table) +
                         static_cast<uintptr_t>(profile.cid_class) * sizeof(uint64_t),
                     &class_class) ||
        !dartplant::RequireCid(reader, profile, class_class, profile.cid_class)) {
        return dartplant::FailProbe("sampled cached ClassTable does not expose Class CID");
    }

    uint64_t libraries = 0;
    if (!reader.Read(
            static_cast<uintptr_t>(context.object_store) + profile.object_store_libraries_offset,
            &libraries) ||
        !dartplant::RequireCid(reader, profile, libraries, profile.cid_growable_object_array)) {
        return dartplant::FailProbe("sampled ObjectStore.libraries is invalid");
    }
    const uintptr_t growable = dartplant::Untag(profile, libraries);
    uint64_t library_count = 0;
    uint64_t library_data = 0;
    if (!dartplant::ReadPositiveCompressedSmi(
            reader, growable + profile.growable_object_array_length_offset, profile,
            &library_count) ||
        library_count == 0 || library_count > dartplant::kMaxClassFunctions ||
        !dartplant::ReadCompressedObject(reader, growable, profile,
                                         profile.growable_object_array_data_offset,
                                         context.heap_base, &library_data)) {
        return dartplant::FailProbe("sampled ObjectStore library array is invalid");
    }
    uint32_t library_data_cid = 0;
    if (!dartplant::ReadCid(reader, profile, library_data, &library_data_cid) ||
        (library_data_cid != profile.cid_array &&
         library_data_cid != profile.cid_immutable_array)) {
        return dartplant::FailProbe("sampled ObjectStore library backing array is invalid");
    }

    bool has_library = false;
    bool has_dart_core = false;
    const uint64_t scan_count = std::min<uint64_t>(library_count, 4096);
    for (uint64_t index = 0; index < scan_count; ++index) {
        uint64_t library = 0;
        if (!dartplant::ReadArrayElement(reader, profile, context.heap_base, library_data, index,
                                         &library) ||
            !dartplant::RequireCid(reader, profile, library, profile.cid_library)) {
            continue;
        }
        has_library = true;
        if (dartplant::LibraryIdentityMatches(reader, profile, context.heap_base, library,
                                              "dart:core")) {
            has_dart_core = true;
            break;
        }
    }
    if (!has_library) {
        return dartplant::FailProbe("sampled ObjectStore contains no valid Library roots");
    }
    if (!has_dart_core) {
        return dartplant::FailProbe("sampled ObjectStore does not contain dart:core");
    }

    *out_context = context;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_probe_invocation(
    const DartPlantInvocation* invocation, const DartPlantFlutterSnapshotInfo* snapshot,
    DartPlantLiveVmProbeInfo* out_info) {
    if (invocation == nullptr || invocation->context == nullptr ||
        invocation->requested_method == nullptr || snapshot == nullptr || out_info == nullptr ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
        out_info->struct_size < dartplant::kLiveVmProbeInfoV1Size) {
        dartplant::SetLastError("live VM probe arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    DartPlantStatus status = dartplant::SelectProfile(*snapshot, &profile);
    if (status != DARTPLANT_OK) return status;
    const dartplant::RawObjectLayout* raw = dartplant::FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr) return dartplant::FailProbe("live VM raw-object profile is unavailable");

    dartplant::ProcessMemoryReader reader;
    if (!reader.Refresh()) {
        return dartplant::FailProbe("cannot read /proc/self/maps for live VM probe");
    }

    DartPlantLiveVmProbeInfo info{};
    info.struct_size = sizeof(info);
    info.profile_version = profile.profile_version;
    info.profile_name = profile.name;

    const DartPlantArm64Context& context = *invocation->context;
    info.thread = dartplant::CanonicalNativePointer(context.x[profile.thr_register]);
    info.pp = context.x[profile.pp_register];
    info.code_register = context.x[profile.code_register];
    const uint64_t heap_bits = context.x[profile.heap_bits_register];
    const uint64_t null_register = context.x[profile.null_register];

    if (info.thread == 0 ||
        !reader.Contains(static_cast<uintptr_t>(info.thread),
                         profile.thread_isolate_group_offset + sizeof(uint64_t))) {
        return dartplant::FailProbe("THR does not point to a readable Dart Thread layout");
    }

    uint64_t thread_pool = 0;
    uint64_t thread_null = 0;
    if (!reader.Read(static_cast<uintptr_t>(info.thread) + profile.thread_heap_base_offset,
                     &info.heap_base) ||
        !reader.Read(static_cast<uintptr_t>(info.thread) + profile.thread_object_null_offset,
                     &thread_null) ||
        !reader.Read(static_cast<uintptr_t>(info.thread) + profile.thread_global_object_pool_offset,
                     &thread_pool) ||
        !dartplant::ReadNativePointer(
            reader, static_cast<uintptr_t>(info.thread) + profile.thread_isolate_offset,
            &info.isolate) ||
        !dartplant::ReadNativePointer(
            reader, static_cast<uintptr_t>(info.thread) + profile.thread_isolate_group_offset,
            &info.isolate_group)) {
        return dartplant::FailProbe("failed to read Dart Thread profile fields");
    }

    info.heap_bits_match =
        static_cast<uint32_t>(heap_bits) == static_cast<uint32_t>(info.heap_base >> 32) ? 1 : 0;
    info.null_register_match = null_register == thread_null ? 1 : 0;
    info.thread_pool_match = dartplant::IsHeapObject(profile, thread_pool) &&
                                     thread_pool >= raw->heap_object_tag &&
                                     info.pp == thread_pool - raw->heap_object_tag
                                 ? 1
                                 : 0;
    info.global_object_pool = thread_pool;
    if (!info.heap_bits_match || !info.null_register_match || !info.thread_pool_match) {
        return dartplant::FailProbe("THR/PP/HEAP_BITS/NULL_REG semantic validation failed");
    }

    if (info.isolate == 0 || info.isolate_group == 0 ||
        !reader.Contains(static_cast<uintptr_t>(info.isolate), sizeof(uint64_t)) ||
        !reader.Contains(static_cast<uintptr_t>(info.isolate_group),
                         profile.isolate_group_object_store_offset + sizeof(uint64_t))) {
        return dartplant::FailProbe("Dart isolate or isolate group pointer is not readable");
    }
    if (!dartplant::ReadNativePointer(
            reader,
            static_cast<uintptr_t>(info.isolate_group) + profile.isolate_group_class_table_offset,
            &info.class_table) ||
        !dartplant::ReadNativePointer(reader,
                                      static_cast<uintptr_t>(info.isolate_group) +
                                          profile.isolate_group_cached_class_table_table_offset,
                                      &info.cached_class_table_table) ||
        !dartplant::ReadNativePointer(
            reader,
            static_cast<uintptr_t>(info.isolate_group) + profile.isolate_group_object_store_offset,
            &info.object_store)) {
        return dartplant::FailProbe("IsolateGroup raw layout validation failed");
    }

    if (!dartplant::RequireCid(reader, profile, info.global_object_pool, profile.cid_object_pool)) {
        return dartplant::FailProbe("THR global object pool does not have ObjectPool CID");
    }
    if (!reader.Read(
            dartplant::Untag(profile, info.global_object_pool) + profile.object_pool_length_offset,
            &info.object_pool_length) ||
        info.object_pool_length == 0 ||
        info.object_pool_length > dartplant::kMaxObjectPoolEntries) {
        return dartplant::FailProbe("live ObjectPool length is invalid");
    }

    const uintptr_t target_entry = dartplant::MethodTarget(invocation->requested_method);
    if (target_entry == 0) {
        return dartplant::FailProbe("live VM probe has no current Dart method entry");
    }

    const char* expected_function = invocation->requested_method->record.function_name.c_str();
    const char* expected_class = invocation->requested_method->record.class_name.c_str();
    info.requested_entry_kind = invocation->requested_method->record.entry_kind;
    uint64_t indexed_function = 0;
    if (!dartplant::FindFunctionByEntryIdentity(
            reader, profile, info.heap_base, static_cast<uintptr_t>(info.class_table),
            static_cast<uintptr_t>(info.cached_class_table_table),
            static_cast<uintptr_t>(info.object_store), target_entry, info.requested_entry_kind,
            expected_function, expected_class, &indexed_function, &info.entry_alias_count)) {
        return dartplant::FailProbe(
            "ClassTable/ObjectStore live index could not resolve current method identity");
    }
    info.function = indexed_function;
    info.function_found_from_vm_index = 1;
    info.entry_is_shared = info.entry_alias_count > 1 ? 1 : 0;
    if (!dartplant::RequireCid(reader, profile, info.function, profile.cid_function)) {
        return dartplant::FailProbe("live VM index returned a non-Function object");
    }

    const uintptr_t function = dartplant::Untag(profile, info.function);
    uint64_t function_name = 0;
    uint64_t function_owner = 0;
    uint64_t function_code = 0;
    if (!reader.Read(function + profile.function_entry_point_offset, &info.function_entry_point) ||
        !dartplant::ReadCompressedObject(reader, function, profile, profile.function_name_offset,
                                         info.heap_base, &function_name) ||
        !dartplant::ReadCompressedObject(reader, function, profile, profile.function_owner_offset,
                                         info.heap_base, &function_owner) ||
        !dartplant::ReadCompressedObject(reader, function, profile, profile.function_code_offset,
                                         info.heap_base, &function_code)) {
        return dartplant::FailProbe("failed to read Dart Function raw fields");
    }
    if (!dartplant::ReadFunctionEntryForKind(reader, profile, info.heap_base, info.function,
                                             info.requested_entry_kind,
                                             &info.selected_entry_point) ||
        info.selected_entry_point != target_entry ||
        !dartplant::ReadDartString(reader, profile, function_name, info.function_name,
                                   sizeof(info.function_name))) {
        return dartplant::FailProbe("Function selected-entry/name semantic validation failed");
    }

    info.code = function_code;
    if (!dartplant::RequireCid(reader, profile, info.code, profile.cid_code)) {
        return dartplant::FailProbe("Function.code is not a Dart Code object");
    }
    uint64_t code_pool = 0;
    if (!reader.Read(dartplant::Untag(profile, info.code) + profile.code_entry_point_offset,
                     &info.code_entry_point) ||
        !reader.Read(dartplant::Untag(profile, info.code) + profile.code_object_pool_offset,
                     &code_pool) ||
        !reader.Read(dartplant::Untag(profile, info.code) + profile.code_owner_offset,
                     &info.code_owner)) {
        return dartplant::FailProbe("failed to read Dart Code raw fields");
    }
    info.function_code_match = info.code_owner == info.function ? 1 : 0;
    info.code_owner_is_function =
        dartplant::RequireCid(reader, profile, info.code_owner, profile.cid_function) ? 1 : 0;
    info.code_owner_mismatch_allowed =
        !info.function_code_match && info.code_owner_is_function &&
                dartplant::HasSnapshotFeature(snapshot->snapshot_features, "dedup_instructions")
            ? 1
            : 0;
    info.code_entry_matches_function = info.code_entry_point == info.function_entry_point ? 1 : 0;
    if (!info.function_code_match && !info.code_owner_mismatch_allowed) {
        return dartplant::FailProbe("Function.code owner mismatch is not explained by dedup");
    }
    // Full-AOT precompiled snapshots intentionally deserialize Code.object_pool
    // as ObjectPool::null(). Code::GetObjectPool() then resolves the effective
    // pool through IsolateGroup::object_store()->global_object_pool(). Keep the
    // raw field distinction visible while validating the effective PP source.
    info.code_pool_is_null = code_pool == thread_null ? 1 : 0;
    info.code_pool_match = info.global_object_pool >= raw->heap_object_tag &&
                                   info.pp == info.global_object_pool - raw->heap_object_tag &&
                                   (info.code_pool_is_null || code_pool == info.global_object_pool)
                               ? 1
                               : 0;
    if (!info.code_pool_match) {
        return dartplant::FailProbe(
            "Code effective ObjectPool does not match PP/global ObjectPool");
    }

    info.owner_class = function_owner;
    if (!dartplant::RequireCid(reader, profile, info.owner_class, profile.cid_class)) {
        return dartplant::FailProbe("Function.owner is not a Dart Class object");
    }
    const uintptr_t owner_class = dartplant::Untag(profile, info.owner_class);
    uint64_t class_name = 0;
    uint64_t class_functions = 0;
    if (!dartplant::ReadCompressedObject(reader, owner_class, profile, profile.class_name_offset,
                                         info.heap_base, &class_name) ||
        !dartplant::ReadCompressedObject(reader, owner_class, profile,
                                         profile.class_functions_offset, info.heap_base,
                                         &class_functions) ||
        !dartplant::ReadCompressedObject(reader, owner_class, profile, profile.class_library_offset,
                                         info.heap_base, &info.library)) {
        return dartplant::FailProbe("failed to read Dart Class raw fields");
    }
    if (!dartplant::ReadDartString(reader, profile, class_name, info.class_name,
                                   sizeof(info.class_name)) ||
        !dartplant::RequireCid(reader, profile, info.library, profile.cid_library)) {
        return dartplant::FailProbe("Class.name/library semantic validation failed");
    }
    info.function_in_class_functions =
        dartplant::ArrayContainsFunction(reader, profile, info.heap_base, class_functions,
                                         info.function)
            ? 1
            : 0;
    if (!info.function_in_class_functions) {
        return dartplant::FailProbe("current Function is absent from Class.functions");
    }

    const uintptr_t library = dartplant::Untag(profile, info.library);
    uint64_t library_url = 0;
    uint64_t top_level_class = 0;
    if (!dartplant::ReadCompressedObject(reader, library, profile, profile.library_url_offset,
                                         info.heap_base, &library_url) ||
        !dartplant::ReadCompressedObject(reader, library, profile,
                                         profile.library_toplevel_class_offset, info.heap_base,
                                         &top_level_class) ||
        !dartplant::ReadDartString(reader, profile, library_url, info.library_uri,
                                   sizeof(info.library_uri))) {
        return dartplant::FailProbe("failed to reconstruct Dart Library URL/top-level class");
    }
    info.owner_is_toplevel_class = top_level_class == info.owner_class ? 1 : 0;
    if (info.owner_is_toplevel_class) {
        std::snprintf(info.class_name, sizeof(info.class_name), "%s", "Global");
    }

    dartplant::CopyOutputPrefix(info, out_info);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_context_from_probe(
    const DartPlantLiveVmProbeInfo* probe, DartPlantLiveVmContext* out_context) {
    if (probe == nullptr || out_context == nullptr ||
        probe->struct_size < dartplant::kLiveVmProbeInfoV1Size ||
        out_context->struct_size < sizeof(DartPlantLiveVmContext)) {
        dartplant::SetLastError("live VM context arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (probe->profile_name == nullptr || probe->thread == 0 || probe->isolate_group == 0 ||
        probe->class_table == 0 || probe->cached_class_table_table == 0 ||
        probe->object_store == 0 || probe->heap_base == 0 || probe->global_object_pool == 0 ||
        !probe->heap_bits_match || !probe->null_register_match || !probe->thread_pool_match) {
        dartplant::SetLastError("live VM probe does not contain a validated reusable context");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    context.profile_version = probe->profile_version;
    context.profile_name = probe->profile_name;
    context.thread = probe->thread;
    context.isolate = probe->isolate;
    context.isolate_group = probe->isolate_group;
    context.class_table = probe->class_table;
    context.cached_class_table_table = probe->cached_class_table_table;
    context.object_store = probe->object_store;
    context.heap_base = probe->heap_base;
    context.pp = probe->pp;
    context.global_object_pool = probe->global_object_pool;
    context.object_pool_length = probe->object_pool_length;
    *out_context = context;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_find_method(
    const DartPlantLiveVmContext* context, const DartPlantFlutterSnapshotInfo* snapshot,
    const char* library_uri, const char* class_name, const char* function_name,
    DartPlantLiveVmMethodInfo* out_method) {
    if (context == nullptr || snapshot == nullptr || library_uri == nullptr ||
        class_name == nullptr || function_name == nullptr || out_method == nullptr ||
        context->struct_size < sizeof(DartPlantLiveVmContext) ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
        out_method->struct_size < sizeof(DartPlantLiveVmMethodInfo)) {
        dartplant::SetLastError("live VM method lookup arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    DartPlantStatus status = dartplant::SelectProfile(*snapshot, &profile);
    if (status != DARTPLANT_OK) return status;
    if (context->profile_version != profile.profile_version ||
        !dartplant::SameString(context->profile_name, profile.name)) {
        dartplant::SetLastError("live VM context profile does not match snapshot profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    dartplant::ProcessMemoryReader reader;
    if (!reader.Refresh()) {
        return dartplant::FailProbe("cannot read /proc/self/maps for live VM method lookup");
    }
    if (context->class_table == 0 || context->cached_class_table_table == 0 ||
        context->object_store == 0 || context->heap_base == 0 ||
        !dartplant::RequireCid(reader, profile, context->global_object_pool,
                               profile.cid_object_pool)) {
        return dartplant::FailProbe("live VM context roots are stale or invalid");
    }

    uint64_t function = 0;
    uint64_t library = 0;
    if (!dartplant::FindFunctionByIdentity(
            reader, profile, context->heap_base, static_cast<uintptr_t>(context->class_table),
            static_cast<uintptr_t>(context->cached_class_table_table),
            static_cast<uintptr_t>(context->object_store), library_uri, class_name, function_name,
            &function, &library)) {
        dartplant::SetLastError("live VM method identity was not found");
        return DARTPLANT_METHOD_NOT_FOUND;
    }

    DartPlantLiveVmMethodInfo method{};
    method.struct_size = sizeof(method);
    method.function = function;
    method.library = library;

    const uintptr_t function_address = dartplant::Untag(profile, function);
    uint64_t tagged_name = 0;
    uint64_t function_owner = 0;
    if (!reader.Read(function_address + profile.function_entry_point_offset,
                     &method.function_entry_point) ||
        !dartplant::ReadCompressedObject(reader, function_address, profile,
                                         profile.function_name_offset, context->heap_base,
                                         &tagged_name) ||
        !dartplant::ReadCompressedObject(reader, function_address, profile,
                                         profile.function_owner_offset, context->heap_base,
                                         &function_owner) ||
        !dartplant::ReadCompressedObject(reader, function_address, profile,
                                         profile.function_code_offset, context->heap_base,
                                         &method.code) ||
        !dartplant::ReadDartString(reader, profile, tagged_name, method.function_name,
                                   sizeof(method.function_name))) {
        return dartplant::FailProbe("failed to reconstruct live VM Function identity");
    }

    method.owner_class = function_owner;
    if (!dartplant::RequireCid(reader, profile, method.owner_class, profile.cid_class) ||
        !dartplant::RequireCid(reader, profile, method.code, profile.cid_code)) {
        return dartplant::FailProbe("live VM Function owner or Code has an invalid CID");
    }

    uint64_t class_name_object = 0;
    uint64_t class_functions = 0;
    uint64_t class_library = 0;
    if (!dartplant::ReadCompressedObject(reader, dartplant::Untag(profile, method.owner_class),
                                         profile, profile.class_name_offset, context->heap_base,
                                         &class_name_object) ||
        !dartplant::ReadCompressedObject(reader, dartplant::Untag(profile, method.owner_class),
                                         profile, profile.class_functions_offset,
                                         context->heap_base, &class_functions) ||
        !dartplant::ReadClassLibrary(reader, profile, context->heap_base, method.owner_class,
                                     &class_library) ||
        class_library != method.library) {
        return dartplant::FailProbe("live VM Class identity is inconsistent");
    }
    if (!dartplant::ReadDartString(reader, profile, class_name_object, method.class_name,
                                   sizeof(method.class_name)) ||
        !dartplant::ReadLibraryUri(reader, profile, context->heap_base, method.library,
                                   method.library_uri, sizeof(method.library_uri))) {
        return dartplant::FailProbe("failed to reconstruct live VM Class or Library name");
    }

    uint64_t top_level_class = 0;
    if (!dartplant::ReadCompressedObject(reader, dartplant::Untag(profile, method.library), profile,
                                         profile.library_toplevel_class_offset, context->heap_base,
                                         &top_level_class)) {
        return dartplant::FailProbe("failed to read live VM Library top-level class");
    }
    method.owner_is_toplevel_class = top_level_class == method.owner_class ? 1 : 0;
    if (method.owner_is_toplevel_class) {
        std::snprintf(method.class_name, sizeof(method.class_name), "%s", "Global");
    }
    method.function_in_class_functions =
        dartplant::ArrayContainsFunction(reader, profile, context->heap_base, class_functions,
                                         method.function)
            ? 1
            : 0;
    if (!method.function_in_class_functions) {
        return dartplant::FailProbe("live VM Function is absent from Class.functions");
    }

    uint64_t code_pool = 0;
    const uintptr_t code = dartplant::Untag(profile, method.code);
    if (!reader.Read(code + profile.code_entry_point_offset, &method.code_entry_point) ||
        !reader.Read(code + profile.code_object_pool_offset, &code_pool) ||
        !reader.Read(code + profile.code_owner_offset, &method.code_owner) ||
        !reader.Read(code + profile.code_instructions_length_offset, &method.code_size) ||
        method.code_size == 0) {
        return dartplant::FailProbe("failed to reconstruct live VM Code");
    }
    method.function_code_owner_match = method.code_owner == method.function ? 1 : 0;
    method.code_owner_is_function =
        dartplant::RequireCid(reader, profile, method.code_owner, profile.cid_function) ? 1 : 0;
    method.code_owner_mismatch_allowed =
        !method.function_code_owner_match && method.code_owner_is_function &&
                dartplant::HasSnapshotFeature(snapshot->snapshot_features, "dedup_instructions")
            ? 1
            : 0;
    method.code_entry_matches_function =
        method.code_entry_point == method.function_entry_point ? 1 : 0;
    if (!method.function_code_owner_match && !method.code_owner_mismatch_allowed) {
        return dartplant::FailProbe("live VM Code owner mismatch is not explained by dedup");
    }

    uint64_t ignored_function = 0;
    if (!dartplant::FindFunctionByEntryIdentity(
            reader, profile, context->heap_base, static_cast<uintptr_t>(context->class_table),
            static_cast<uintptr_t>(context->cached_class_table_table),
            static_cast<uintptr_t>(context->object_store),
            static_cast<uintptr_t>(method.function_entry_point), DARTPLANT_ENTRY_DEFAULT,
            function_name, class_name, &ignored_function, &method.entry_alias_count)) {
        return dartplant::FailProbe("live VM entry alias scan failed");
    }
    method.entry_is_shared = method.entry_alias_count > 1 ? 1 : 0;

    *out_method = method;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_read_function_signature(
    const DartPlantLiveVmContext* context, const DartPlantFlutterSnapshotInfo* snapshot,
    uint64_t function, DartPlantDartFunctionSignatureInfo* out_signature) {
    if (context == nullptr || snapshot == nullptr || out_signature == nullptr ||
        context->struct_size < sizeof(DartPlantLiveVmContext) ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
        out_signature->struct_size < sizeof(DartPlantDartFunctionSignatureInfo)) {
        dartplant::SetLastError("live VM FunctionType signature arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    DartPlantLiveVmProfile profile{};
    const dartplant::FunctionTypeLayout* layout = nullptr;
    dartplant::ProcessMemoryReader reader;
    DartPlantStatus status = dartplant::PrepareFunctionSignatureRead(*context, *snapshot, function,
                                                                     &profile, &layout, &reader);
    if (status != DARTPLANT_OK) return status;

    dartplant::ParsedFunctionSignature parsed{};
    status = dartplant::ParseRetainedFunctionSignature(reader, profile, *layout, context->heap_base,
                                                       function, &parsed);
    if (status != DARTPLANT_OK) return status;

    DartPlantDartFunctionSignatureInfo signature{};
    signature.struct_size = sizeof(signature);
    signature.parameter_count = parsed.parameter_count;
    signature.implicit_parameter_count = parsed.implicit_parameter_count;
    signature.fixed_parameter_count = parsed.fixed_parameter_count;
    signature.optional_parameter_count = parsed.optional_parameter_count;
    signature.type_parameter_count = parsed.type_parameter_count;
    signature.parent_type_argument_count = parsed.parent_type_argument_count;
    signature.has_named_optional_parameters = parsed.has_named_optional_parameters ? 1 : 0;
    signature.result_type = parsed.result_type;
    *out_signature = signature;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_read_function_parameter(
    const DartPlantLiveVmContext* context, const DartPlantFlutterSnapshotInfo* snapshot,
    uint64_t function, uint32_t index, DartPlantDartParameterInfo* out_parameter) {
    if (context == nullptr || snapshot == nullptr || out_parameter == nullptr ||
        context->struct_size < sizeof(DartPlantLiveVmContext) ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
        out_parameter->struct_size < sizeof(DartPlantDartParameterInfo)) {
        dartplant::SetLastError("live VM FunctionType parameter arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    DartPlantLiveVmProfile profile{};
    const dartplant::FunctionTypeLayout* layout = nullptr;
    dartplant::ProcessMemoryReader reader;
    DartPlantStatus status = dartplant::PrepareFunctionSignatureRead(*context, *snapshot, function,
                                                                     &profile, &layout, &reader);
    if (status != DARTPLANT_OK) return status;

    dartplant::ParsedFunctionSignature parsed{};
    status = dartplant::ParseRetainedFunctionSignature(reader, profile, *layout, context->heap_base,
                                                       function, &parsed);
    if (status != DARTPLANT_OK) return status;
    if (index >= parsed.parameter_count) {
        dartplant::SetLastError("FunctionType parameter index is out of range");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    uint64_t tagged_type = 0;
    if (!dartplant::ReadArrayElement(reader, profile, context->heap_base, parsed.parameter_types,
                                     index, &tagged_type)) {
        return dartplant::FailProbe("FunctionType parameter type is unreadable");
    }

    DartPlantDartParameterInfo parameter{};
    parameter.struct_size = sizeof(parameter);
    parameter.index = index;
    if (!dartplant::DecodeDartType(reader, profile, *layout, tagged_type, &parameter.type)) {
        return dartplant::FailProbe("FunctionType parameter type is invalid");
    }

    if (index < parsed.implicit_parameter_count) {
        parameter.kind = DARTPLANT_DART_PARAMETER_IMPLICIT;
    } else if (index < parsed.fixed_parameter_count) {
        parameter.kind = DARTPLANT_DART_PARAMETER_REQUIRED_POSITIONAL;
        parameter.is_required = 1;
    } else if (!parsed.has_named_optional_parameters) {
        parameter.kind = DARTPLANT_DART_PARAMETER_OPTIONAL_POSITIONAL;
    } else {
        parameter.kind = DARTPLANT_DART_PARAMETER_NAMED;
        const uint32_t named_index = index - parsed.fixed_parameter_count;
        uint64_t tagged_name = 0;
        if (!dartplant::ReadArrayElement(reader, profile, context->heap_base,
                                         parsed.named_parameter_names, named_index, &tagged_name) ||
            !dartplant::ReadDartString(reader, profile, tagged_name, parameter.name,
                                       sizeof(parameter.name))) {
            return dartplant::FailProbe("FunctionType named parameter name is invalid");
        }

        // FunctionType::GetRequiredFlagIndex(): required flags are appended to
        // named_parameter_names as Smi bitmaps.
        const uint32_t flag_index =
            parsed.optional_parameter_count + named_index / dartplant::kNamedParameterFlagsPerSmi;
        uint64_t named_slot_count = 0;
        if (!dartplant::ReadArrayLength(reader, profile, parsed.named_parameter_names,
                                        &named_slot_count)) {
            return dartplant::FailProbe("FunctionType named parameter flags are unreadable");
        }
        if (flag_index < named_slot_count) {
            const dartplant::RawObjectLayout* raw =
                dartplant::FindRawObjectLayout(profile.profile_version);
            uint32_t raw_flags = 0;
            if (raw == nullptr ||
                !dartplant::ReadArrayRawElement(reader, profile, parsed.named_parameter_names,
                                                flag_index, &raw_flags) ||
                (raw_flags & raw->smi_tag_mask) != raw->smi_tag) {
                return dartplant::FailProbe("FunctionType required-named flags are not a Smi");
            }
            const uint32_t flags = raw_flags >> raw->smi_tag_shift;
            const uint32_t mask = 1U << (named_index % dartplant::kNamedParameterFlagsPerSmi);
            parameter.is_required = (flags & mask) != 0 ? 1 : 0;
        }
    }

    *out_parameter = parameter;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_visit_functions(
    const DartPlantLiveVmContext* context, const DartPlantFlutterSnapshotInfo* snapshot,
    DartPlantLiveVmFunctionVisitor visitor, void* user_data,
    DartPlantLiveVmFunctionIndexInfo* out_info) {
    if (context == nullptr || snapshot == nullptr || visitor == nullptr || out_info == nullptr ||
        context->struct_size < sizeof(DartPlantLiveVmContext) ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
        out_info->struct_size < sizeof(DartPlantLiveVmFunctionIndexInfo)) {
        dartplant::SetLastError("live VM function enumeration arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    DartPlantStatus status = dartplant::SelectProfile(*snapshot, &profile);
    if (status != DARTPLANT_OK) return status;
    if (context->profile_version != profile.profile_version ||
        !dartplant::SameString(context->profile_name, profile.name)) {
        dartplant::SetLastError("live VM context profile does not match function index profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    dartplant::ProcessMemoryReader reader;
    if (!reader.Refresh()) {
        return dartplant::FailProbe("cannot read /proc/self/maps for live Function index");
    }
    std::vector<dartplant::CollectedLiveFunction> functions;
    DartPlantLiveVmFunctionIndexInfo info{};
    info.struct_size = sizeof(info);
    if (!dartplant::CollectAllLiveFunctions(reader, profile, *context, *snapshot, &functions,
                                            &info)) {
        return dartplant::FailProbe("failed to enumerate live Dart Function graph");
    }
    for (const auto& function : functions) {
        if (!visitor(&function.info, user_data)) break;
    }
    *out_info = info;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_object_pool_offset_from_index(
    const DartPlantFlutterSnapshotInfo* snapshot, uint32_t index, uint64_t* out_offset) {
    if (snapshot == nullptr || out_offset == nullptr ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo)) {
        dartplant::SetLastError("ObjectPool offset conversion arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    const DartPlantStatus status = dartplant::SelectProfile(*snapshot, &profile);
    if (status != DARTPLANT_OK) return status;
    *out_offset = dartplant::ObjectPoolOffsetFromIndex(profile, index);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_object_pool_index_from_offset(
    const DartPlantFlutterSnapshotInfo* snapshot, uint64_t offset, uint32_t* out_index) {
    if (snapshot == nullptr || out_index == nullptr ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo)) {
        dartplant::SetLastError("ObjectPool index conversion arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    const DartPlantStatus status = dartplant::SelectProfile(*snapshot, &profile);
    if (status != DARTPLANT_OK) return status;
    if (!dartplant::ObjectPoolIndexFromOffset(profile, offset, out_index)) {
        dartplant::SetLastError("ObjectPool byte offset is not a valid VM pool index");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_read_object_pool_entry(
    const DartPlantLiveVmContext* context, const DartPlantFlutterSnapshotInfo* snapshot,
    uint64_t tagged_object_pool, uint32_t index, DartPlantObjectPoolEntryInfo* out_entry) {
    if (context == nullptr || snapshot == nullptr || out_entry == nullptr ||
        context->struct_size < sizeof(DartPlantLiveVmContext) ||
        snapshot->struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
        out_entry->struct_size < sizeof(DartPlantObjectPoolEntryInfo)) {
        dartplant::SetLastError("ObjectPool entry arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    DartPlantStatus status = dartplant::SelectProfile(*snapshot, &profile);
    if (status != DARTPLANT_OK) return status;
    if (context->profile_version != profile.profile_version ||
        !dartplant::SameString(context->profile_name, profile.name)) {
        dartplant::SetLastError("live VM context profile does not match ObjectPool profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    dartplant::ProcessMemoryReader reader;
    uint32_t pool_cid = 0;
    if (!reader.Refresh() ||
        !dartplant::ReadCidSafely(reader, profile, tagged_object_pool, &pool_cid) ||
        pool_cid != profile.cid_object_pool) {
        return dartplant::FailProbe("ObjectPool is stale or has an invalid CID");
    }
    const uintptr_t pool = dartplant::Untag(profile, tagged_object_pool);
    uint64_t length = 0;
    if (!reader.ReadSafely(pool + profile.object_pool_length_offset, &length) ||
        length > dartplant::kMaxObjectPoolEntries || index >= length) {
        dartplant::SetLastError("ObjectPool index is out of range");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    const uintptr_t data_start = pool + profile.object_pool_elements_offset;
    uint64_t raw = 0;
    uint8_t bits = 0;
    if (!reader.ReadSafely(data_start + static_cast<uintptr_t>(index) * sizeof(uint64_t), &raw) ||
        !reader.ReadSafely(data_start + static_cast<uintptr_t>(length) * sizeof(uint64_t) + index,
                           &bits)) {
        return dartplant::FailProbe("ObjectPool entry data or type bits are unreadable");
    }

    DartPlantObjectPoolEntryInfo entry{};
    entry.struct_size = sizeof(entry);
    entry.index = index;
    entry.pool = tagged_object_pool;
    entry.raw_value = raw;
    entry.byte_offset = dartplant::ObjectPoolOffsetFromIndex(profile, index);
    entry.entry_bits = bits;
    const uint8_t type = bits & 0x0f;
    entry.type = type <= DARTPLANT_OBJECT_POOL_NATIVE_FUNCTION
                     ? static_cast<DartPlantObjectPoolEntryType>(type)
                     : DARTPLANT_OBJECT_POOL_UNKNOWN;
    // ObjectPoolBuilder::Patchability is intentionally encoded as
    // kPatchable=0, kNotPatchable=1. Expose a normal boolean to callers.
    entry.patchable = static_cast<uint8_t>(((bits >> 4) & 0x1) == 0);
    entry.snapshot_behavior = static_cast<uint8_t>((bits >> 5) & 0x7);
    if (entry.type == DARTPLANT_OBJECT_POOL_TAGGED_OBJECT) {
        entry.tagged_object = raw;
        if (dartplant::IsHeapObject(profile, raw)) {
            (void) dartplant::ReadCidSafely(reader, profile, raw, &entry.object_cid);
        }
    }
    *out_entry = entry;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}
