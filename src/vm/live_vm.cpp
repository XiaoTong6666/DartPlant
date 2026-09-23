// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "dartplant/advanced/live_vm.h"

#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "android_logging.h"
#include "runtime/runtime_internal.h"
#include "vm/abi/probe.h"
#include "vm/abi/proof.h"
#include "vm/abi/resolver.h"
#include "vm/dart_string.h"
#include "vm/live_vm_internal.h"
#include "vm/object_bridge.h"
#include "vm/runtime_profiles.h"

namespace dartplant {
namespace {

constexpr uint64_t kMaxObjectPoolEntries = 1ULL << 24;
constexpr uint64_t kMaxClassFunctions = 1ULL << 20;
// Stable owner ids share one uint32_t namespace. Ordinary Classes use their
// ClassTable slot id. Top-level Classes are not required to have a ClassTable
// slot, so reserve bit 31 and encode their ObjectStore.libraries slot.
// Deferred implicit/synthetic Functions may not occur in Class.functions at
// all; reserve bit 30 for an InstructionsTable loading-unit identity and pair
// it with code_objects[index]. Current VM class ids are bounded by
// class_id_tag_bits (20 in supported profiles), leaving all three namespaces
// disjoint by construction.
constexpr uint32_t kTopLevelOwnerIdBit = uint32_t{1} << 31;
constexpr uint32_t kInstructionsTableOwnerIdBit = uint32_t{1} << 30;
constexpr uint32_t kVolatileHeapReadAttempts = 3;
constexpr size_t kLiveVmProfileV1Size =
    offsetof(DartPlantLiveVmProfile, code_unchecked_entry_point_offset);
constexpr size_t kLiveVmProbeInfoV1Size = offsetof(DartPlantLiveVmProbeInfo, requested_entry_kind);

void LogLiveIndex(const char* format, ...) {
#if defined(__ANDROID__)
    va_list args;
    va_start(args, format);
    AndroidLogVPrint(ANDROID_LOG_INFO, "LiveIndex", format, args);
    va_end(args);
#else
    (void) format;
#endif
}

bool FailClassCollection(const char** out_stage, const char* stage) {
    if (out_stage != nullptr) *out_stage = stage;
    return false;
}

bool FailFunctionCollection(const char** out_stage, const char* stage) {
    if (out_stage != nullptr) *out_stage = stage;
    return false;
}

std::optional<uint32_t> StableTopLevelOwnerId(uint64_t library_index) {
    if (library_index >= kTopLevelOwnerIdBit - 1) return std::nullopt;
    return kTopLevelOwnerIdBit | (static_cast<uint32_t>(library_index) + 1u);
}

std::optional<uint32_t> StableInstructionsTableOwnerId(uint32_t loading_unit_id) {
    if (loading_unit_id <= 1 || loading_unit_id >= kInstructionsTableOwnerIdBit) {
        return std::nullopt;
    }
    return kInstructionsTableOwnerIdBit | loading_unit_id;
}

template <typename T>
void CopyOutputPrefix(const T& source, T* destination) {
    const size_t caller_size = destination->struct_size;
    const size_t written_size = std::min(caller_size, sizeof(T));
    std::memcpy(destination, &source, written_size);
    destination->struct_size = static_cast<uint32_t>(written_size);
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
    static ProcessMemoryReader VolatileSafe() { return ProcessMemoryReader(true); }

    static std::optional<ProcessMemoryReader> ObservationScopedDirect(
        DartPlantVmAdapter* adapter, uint64_t thread, const void* observation_lease) {
        if (!VmAdapterOwnsLiveHeapObservation(adapter, thread, observation_lease)) {
            return std::nullopt;
        }
        return ProcessMemoryReader(false);
    }

    bool volatile_reads() const { return volatile_reads_; }
    uint64_t read_calls() const { return read_calls_; }
    uint64_t safe_read_calls() const { return safe_read_calls_; }
    uint64_t bytes_read() const { return bytes_read_; }

    bool Refresh() {
        ranges_.clear();
        cached_range_begin_ = 0;
        cached_range_end_ = 0;
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
        // Live-VM traversal performs hundreds of thousands of tiny reads and
        // usually stays inside the same large Dart heap/code mapping for long
        // runs. Keep the full /proc/self/maps validation contract, but avoid a
        // binary search when the immediately previous readable range still
        // contains this access.
        if (cached_range_begin_ < cached_range_end_ && address >= cached_range_begin_ &&
            end <= cached_range_end_) {
            return true;
        }
        auto it = std::upper_bound(
            ranges_.begin(), ranges_.end(), address,
            [](uintptr_t value, const MemoryRange& range) { return value < range.begin; });
        if (it == ranges_.begin()) return false;
        --it;
        if (address < it->begin || end > it->end) return false;
        cached_range_begin_ = it->begin;
        cached_range_end_ = it->end;
        return true;
    }

    template <typename T>
    bool Read(uintptr_t address, T* out_value) const {
        // Keep the raw pointer precondition explicit here instead of relying on
        // Contains() to imply it. Besides being the correct contract for a
        // process-memory reader, this lets clang's path-sensitive analyzer
        // prove that memcpy never receives a null source pointer.
        if (address == 0 || out_value == nullptr || !Contains(address, sizeof(T))) return false;
        ++read_calls_;
        bytes_read_ += sizeof(T);
        if (volatile_reads_) return ReadSafelyUnchecked(address, out_value, sizeof(T));
        std::memcpy(out_value, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }

    bool ReadBytes(uintptr_t address, void* output, size_t size) const {
        if (address == 0 || output == nullptr || !Contains(address, size)) return false;
        ++read_calls_;
        bytes_read_ += size;
        if (volatile_reads_) return ReadSafelyUnchecked(address, output, size);
        std::memcpy(output, reinterpret_cast<const void*>(address), size);
        return true;
    }

    // Managed Dart heap mappings can change after /proc/self/maps is sampled.
    // process_vm_readv() turns that race into EFAULT/short-read instead of a
    // synchronous SIGSEGV in diagnostic APIs that inspect volatile VM memory.
    bool ReadSafely(uintptr_t address, void* output, size_t size) const {
        if (address == 0 || output == nullptr || !Contains(address, size)) return false;
        ++read_calls_;
        bytes_read_ += size;
        return ReadSafelyUnchecked(address, output, size);
    }

    template <typename T>
    bool ReadSafely(uintptr_t address, T* out_value) const {
        return ReadSafely(address, out_value, sizeof(T));
    }

private:
    explicit ProcessMemoryReader(bool volatile_reads) : volatile_reads_(volatile_reads) {}

    bool ReadSafelyUnchecked(uintptr_t address, void* output, size_t size) const {
#if defined(__linux__) && defined(SYS_process_vm_readv)
        ++safe_read_calls_;
        iovec local = {.iov_base = output, .iov_len = size};
        iovec remote = {.iov_base = reinterpret_cast<void*>(address), .iov_len = size};
        const long result = syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0);
        return result == static_cast<long>(size);
#else
        (void) address;
        (void) output;
        (void) size;
        return false;
#endif
    }

    bool volatile_reads_ = false;
    mutable uint64_t read_calls_ = 0;
    mutable uint64_t safe_read_calls_ = 0;
    mutable uint64_t bytes_read_ = 0;
    mutable uintptr_t cached_range_begin_ = 0;
    mutable uintptr_t cached_range_end_ = 0;
    std::vector<MemoryRange> ranges_;
};

constexpr size_t kMaxObjectFieldWindowBytes = 256;

struct ObjectFieldWindow {
    uint32_t begin_offset = 0;
    size_t size = 0;
    std::array<uint8_t, kMaxObjectFieldWindowBytes> bytes{};

    template <typename T>
    bool Load(uint32_t field_offset, T* out_value) const {
        if (out_value == nullptr || field_offset < begin_offset) return false;
        const size_t relative = static_cast<size_t>(field_offset - begin_offset);
        if (relative > size || sizeof(T) > size - relative) return false;
        std::memcpy(out_value, bytes.data() + relative, sizeof(T));
        return true;
    }
};

template <size_t N>
bool ReadObjectFieldWindow(const ProcessMemoryReader& reader, uintptr_t object_address,
                           const std::array<std::pair<uint32_t, size_t>, N>& fields,
                           ObjectFieldWindow* out_window) {
    if (object_address == 0 || out_window == nullptr || fields.empty()) return false;
    uint32_t begin = UINT32_MAX;
    uint64_t end = 0;
    for (const auto& [offset, width] : fields) {
        if (width == 0 || width > kMaxObjectFieldWindowBytes) return false;
        begin = std::min(begin, offset);
        const uint64_t field_end = static_cast<uint64_t>(offset) + width;
        if (field_end > UINT32_MAX || field_end > end) end = field_end;
    }
    if (begin == UINT32_MAX || end <= begin || end - begin > kMaxObjectFieldWindowBytes ||
        object_address > std::numeric_limits<uintptr_t>::max() - begin) {
        return false;
    }
    ObjectFieldWindow window;
    window.begin_offset = begin;
    window.size = static_cast<size_t>(end - begin);
    if (!reader.ReadBytes(object_address + begin, window.bytes.data(), window.size)) return false;
    *out_window = std::move(window);
    return true;
}

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

uint64_t DecompressObject(uint64_t heap_base, uint32_t compressed) {
    return heap_base + static_cast<uint64_t>(compressed);
}

bool DecodeCompressedObjectField(const ObjectFieldWindow& window, uint32_t field_offset,
                                 const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                                 uint64_t* out_tagged) {
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (out_tagged == nullptr || raw == nullptr || raw->compressed_word_size != sizeof(uint32_t)) {
        return false;
    }
    uint32_t compressed = 0;
    if (!window.Load(field_offset, &compressed) ||
        (compressed & raw->smi_tag_mask) != raw->heap_object_tag) {
        return false;
    }
    *out_tagged = DecompressObject(heap_base, compressed);
    return true;
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
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr) return false;
    RuntimeProfileRecord record{};
    record.live_vm = profile;
    record.raw_object = *raw;
    return vm_abi::ReadDartStringUtf8(
        record, tagged,
        [&reader](uintptr_t address, void* output_bytes, size_t size) {
            return reader.ReadBytes(address, output_bytes, size);
        },
        output, capacity);
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

enum class ArrayVisitDecision {
    kContinue,
    kStop,
    kFail,
};

template <typename Visitor>
bool VisitArrayRawElements(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                           uint64_t tagged_array, uint64_t max_length, Visitor&& visitor,
                           uint64_t* out_length = nullptr) {
    uint32_t cid = 0;
    if (!ReadCid(reader, profile, tagged_array, &cid) ||
        (cid != profile.cid_array && cid != profile.cid_immutable_array)) {
        return false;
    }
    const uintptr_t array = Untag(profile, tagged_array);
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, array + profile.array_length_offset, profile, &length) ||
        length > max_length) {
        return false;
    }
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr || raw->compressed_word_size != sizeof(uint32_t)) return false;
    if (out_length != nullptr) *out_length = length;

    // Array payloads are contiguous compressed words. Validate the Array once,
    // then read the already-bounded payload in fixed chunks. The previous hot
    // loops called ReadArrayElement() for every slot, redundantly rereading the
    // same CID and length two extra times per element while holding the exact
    // moving-GC observation receipt.
    constexpr size_t kChunkSlots = 1024;
    std::array<uint32_t, kChunkSlots> slots{};
    const uintptr_t elements = array + profile.array_elements_offset;
    for (uint64_t chunk_start = 0; chunk_start < length; chunk_start += kChunkSlots) {
        const size_t chunk_count =
            static_cast<size_t>(std::min<uint64_t>(kChunkSlots, length - chunk_start));
        const uintptr_t chunk_address = elements + chunk_start * raw->compressed_word_size;
        if (!reader.ReadBytes(chunk_address, slots.data(),
                              chunk_count * raw->compressed_word_size)) {
            return false;
        }
        for (size_t offset = 0; offset < chunk_count; ++offset) {
            switch (visitor(chunk_start + offset, slots[offset])) {
            case ArrayVisitDecision::kContinue:
                break;
            case ArrayVisitDecision::kStop:
                return true;
            case ArrayVisitDecision::kFail:
                return false;
            }
        }
    }
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

using LiveLibraryUriCache = std::unordered_map<uint64_t, std::string>;

// Observation-local only. tagged_library is movable and is therefore valid as
// a cache key only while the exact live-heap observation lease remains held.
// Keeping this cache inside one traversal avoids rereading the same Library
// CID, url field, and Dart String for every Class owned by that Library.
bool ReadCachedLibraryUri(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                          uint64_t heap_base, uint64_t tagged_library, LiveLibraryUriCache* cache,
                          char* output, size_t capacity) {
    if (cache == nullptr || output == nullptr || capacity == 0) return false;
    const auto found = cache->find(tagged_library);
    if (found != cache->end()) {
        if (found->second.size() >= capacity) return false;
        std::memcpy(output, found->second.c_str(), found->second.size() + 1);
        return true;
    }
    char uri[DARTPLANT_LIVE_VM_LIBRARY_URI_MAX] = {};
    if (!ReadLibraryUri(reader, profile, heap_base, tagged_library, uri, sizeof(uri))) {
        return false;
    }
    const auto [inserted, ok] = cache->emplace(tagged_library, uri);
    if (!ok || inserted->second.size() >= capacity) return false;
    std::memcpy(output, inserted->second.c_str(), inserted->second.size() + 1);
    return true;
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

void FinalizeCollectedLiveFunctionAliases(std::vector<CollectedLiveFunction>* functions,
                                          uint32_t skipped,
                                          DartPlantLiveVmFunctionIndexInfo* out_info) {
    if (functions == nullptr || out_info == nullptr) return;
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
}

const LiveVmInstructionImage* ResolveRuntimeInstructionImage(
    std::span<const LiveVmInstructionImage> images, uint64_t entry, uint64_t size,
    uint64_t* out_va) {
    if (out_va == nullptr || entry == 0 || size == 0 || size > SIZE_MAX) return nullptr;
    const LiveVmInstructionImage* selected = nullptr;
    uint64_t selected_va = 0;
    for (const auto& image : images) {
        const auto& snapshot = image.snapshot;
        if (snapshot.isolate_instructions_size == 0 ||
            entry < snapshot.isolate_instructions_runtime) {
            continue;
        }
        const uint64_t offset = entry - snapshot.isolate_instructions_runtime;
        if (offset >= snapshot.isolate_instructions_size ||
            size > snapshot.isolate_instructions_size - offset ||
            snapshot.isolate_instructions_va > std::numeric_limits<uint64_t>::max() - offset) {
            continue;
        }
        if (selected != nullptr) return nullptr;
        selected = &image;
        selected_va = snapshot.isolate_instructions_va + offset;
    }
    if (selected == nullptr) return nullptr;
    *out_va = selected_va;
    return selected;
}

// The caller must have already proved tagged_function is a Function under the
// same observation receipt. Both current callers obtain it from a bounded VM
// owner container and perform that proof immediately before entering here.
// Avoiding a second CID read matters on non-PRODUCT heaps with thousands of
// retained Functions while preserving the exact same proof boundary.
bool CollectProvenLiveFunction(const ProcessMemoryReader& reader,
                               const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                               uint64_t tagged_function, uint32_t kind, uint64_t tagged_class,
                               uint64_t library, bool is_top_level, uint32_t owner_class_id,
                               uint32_t owner_function_index, const char* library_uri,
                               const char* class_name,
                               const DartPlantFlutterSnapshotInfo& root_snapshot,
                               std::span<const LiveVmInstructionImage> images,
                               std::vector<CollectedLiveFunction>* functions,
                               const char** out_stage) {
    if (out_stage != nullptr) *out_stage = "function-cid";
    if (functions == nullptr || library_uri == nullptr || class_name == nullptr) {
        return FailFunctionCollection(out_stage, "function-cid");
    }

    const uintptr_t function_address = Untag(profile, tagged_function);
    CollectedLiveFunction collected{};
    collected.info.struct_size = sizeof(collected.info);
    collected.info.function = tagged_function;
    collected.info.owner_class = tagged_class;
    collected.info.library = library;
    collected.info.owner_is_toplevel_class = is_top_level ? 1 : 0;
    collected.info.function_kind = kind;
    collected.info.owner_class_id = owner_class_id;
    collected.info.owner_function_index = owner_function_index;
    std::snprintf(collected.info.library_uri, sizeof(collected.info.library_uri), "%s",
                  library_uri);
    std::snprintf(collected.info.class_name, sizeof(collected.info.class_name), "%s", class_name);

    uint64_t tagged_name = 0;
    uint64_t function_owner = 0;
    ObjectFieldWindow function_fields;
    const std::array function_field_layout = {
        std::pair{profile.function_entry_point_offset, sizeof(uint64_t)},
        std::pair{profile.function_unchecked_entry_point_offset, sizeof(uint64_t)},
        std::pair{profile.function_name_offset, sizeof(uint32_t)},
        std::pair{profile.function_owner_offset, sizeof(uint32_t)},
        std::pair{profile.function_code_offset, sizeof(uint32_t)},
    };
    if (!ReadObjectFieldWindow(reader, function_address, function_field_layout, &function_fields) ||
        !function_fields.Load(profile.function_entry_point_offset,
                              &collected.info.function_entry_point) ||
        !function_fields.Load(profile.function_unchecked_entry_point_offset,
                              &collected.info.function_unchecked_entry_point) ||
        !DecodeCompressedObjectField(function_fields, profile.function_name_offset, profile,
                                     heap_base, &tagged_name) ||
        !ReadDartString(reader, profile, tagged_name, collected.info.function_name,
                        sizeof(collected.info.function_name)) ||
        !DecodeCompressedObjectField(function_fields, profile.function_owner_offset, profile,
                                     heap_base, &function_owner) ||
        function_owner != tagged_class ||
        !DecodeCompressedObjectField(function_fields, profile.function_code_offset, profile,
                                     heap_base, &collected.info.code) ||
        !RequireCid(reader, profile, collected.info.code, profile.cid_code)) {
        return FailFunctionCollection(out_stage, "function-fields");
    }

    const bool closure_call_entry_only = IsClosureFunctionKind(profile.profile_version, kind);
    uint64_t code_owner = 0;
    const uintptr_t code_address = Untag(profile, collected.info.code);
    ObjectFieldWindow code_fields;
    const std::array code_field_layout = {
        std::pair{profile.code_entry_point_offset, sizeof(uint64_t)},
        std::pair{profile.code_unchecked_entry_point_offset, sizeof(uint64_t)},
        std::pair{profile.code_monomorphic_entry_point_offset, sizeof(uint64_t)},
        std::pair{profile.code_monomorphic_unchecked_entry_point_offset, sizeof(uint64_t)},
        std::pair{profile.code_object_pool_offset, sizeof(uint64_t)},
        std::pair{profile.code_owner_offset, sizeof(uint64_t)},
        std::pair{profile.code_instructions_length_offset, sizeof(uint64_t)},
    };
    if (!ReadObjectFieldWindow(reader, code_address, code_field_layout, &code_fields) ||
        !code_fields.Load(profile.code_entry_point_offset, &collected.info.code_entry_point) ||
        !code_fields.Load(profile.code_unchecked_entry_point_offset,
                          &collected.info.code_unchecked_entry_point) ||
        !code_fields.Load(profile.code_monomorphic_entry_point_offset,
                          &collected.info.code_monomorphic_entry_point) ||
        !code_fields.Load(profile.code_monomorphic_unchecked_entry_point_offset,
                          &collected.info.code_monomorphic_unchecked_entry_point) ||
        !code_fields.Load(profile.code_object_pool_offset, &collected.info.code_object_pool) ||
        !code_fields.Load(profile.code_owner_offset, &code_owner) ||
        !code_fields.Load(profile.code_instructions_length_offset, &collected.info.code_size) ||
        collected.info.function_entry_point == 0 || collected.info.code_entry_point == 0 ||
        collected.info.code_size == 0) {
        return FailFunctionCollection(out_stage, "code-fields");
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
        return FailFunctionCollection(out_stage, "entry-match");
    }
    collected.info.code_owner_matches_function = code_owner == tagged_function ? 1 : 0;
    if (!collected.info.code_owner_matches_function &&
        (!RequireCid(reader, profile, code_owner, profile.cid_function) ||
         !HasSnapshotFeature(root_snapshot.snapshot_features, "dedup_instructions"))) {
        return FailFunctionCollection(out_stage, "code-owner");
    }

    AotCodePayloadRange payload_range{};
    if (!ComputeAotCodePayloadRange(profile.profile_version, collected.info.code_entry_point,
                                    collected.info.code_monomorphic_entry_point,
                                    collected.info.code_size, &payload_range)) {
        return FailFunctionCollection(out_stage, "payload-range");
    }
    const uint64_t payload_start = payload_range.start;
    const uint64_t payload_end = payload_range.end;
    if (collected.info.code_unchecked_entry_point != 0 &&
        (collected.info.code_unchecked_entry_point < payload_start ||
         collected.info.code_unchecked_entry_point >= payload_end)) {
        return FailFunctionCollection(out_stage, "unchecked-range");
    }
    if (collected.info.code_monomorphic_unchecked_entry_point != 0 &&
        (collected.info.code_monomorphic_unchecked_entry_point < payload_start ||
         collected.info.code_monomorphic_unchecked_entry_point >= payload_end)) {
        return FailFunctionCollection(out_stage, "mono-unchecked-range");
    }
    if (collected.info.code_unchecked_entry_point != 0 &&
        collected.info.code_monomorphic_unchecked_entry_point != 0) {
        if (collected.info.code_unchecked_entry_point < collected.info.code_entry_point ||
            collected.info.code_monomorphic_unchecked_entry_point <
                collected.info.code_monomorphic_entry_point) {
            return FailFunctionCollection(out_stage, "unchecked-order");
        }
        const uint64_t unchecked_delta =
            collected.info.code_unchecked_entry_point - collected.info.code_entry_point;
        if (collected.info.code_monomorphic_unchecked_entry_point -
                collected.info.code_monomorphic_entry_point !=
            unchecked_delta) {
            return FailFunctionCollection(out_stage, "unchecked-delta");
        }
    }
    uint64_t default_entry_va = 0;
    const LiveVmInstructionImage* entry_image = ResolveRuntimeInstructionImage(
        images, collected.info.function_entry_point, 4, &default_entry_va);
    if (entry_image == nullptr) {
        return FailFunctionCollection(out_stage, "entry-va");
    }
    collected.info.entry_va = default_entry_va;
    if (!closure_call_entry_only &&
        (ResolveRuntimeInstructionImage(images, collected.info.function_unchecked_entry_point, 4,
                                        &collected.info.unchecked_entry_va) != entry_image ||
         ResolveRuntimeInstructionImage(images, collected.info.code_monomorphic_entry_point, 4,
                                        &collected.info.monomorphic_entry_va) != entry_image ||
         ResolveRuntimeInstructionImage(
             images, collected.info.code_monomorphic_unchecked_entry_point, 4,
             &collected.info.monomorphic_unchecked_entry_va) != entry_image)) {
        return FailFunctionCollection(out_stage, "alternate-entry-va");
    }
    uint64_t payload_va = 0;
    if (ResolveRuntimeInstructionImage(images, payload_start, collected.info.code_size,
                                       &payload_va) != entry_image) {
        return FailFunctionCollection(out_stage, "payload-image");
    }
    collected.info.closure_call_entry_only = closure_call_entry_only ? 1 : 0;
    collected.info.entry_kind_mask = closure_call_entry_only ? 0x1u : 0x0fu;
    collected.info.code_section_va = entry_image->snapshot.isolate_instructions_va;
    collected.info.runtime_image_id = entry_image->runtime_image_id;
    collected.info.runtime_image_incarnation_epoch = entry_image->runtime_image_incarnation_epoch;
    collected.info.engine_incarnation_epoch = entry_image->engine_incarnation_epoch;
    collected.info.isolate_group_incarnation_epoch = entry_image->isolate_group_incarnation_epoch;
    collected.info.runtime_generation = entry_image->runtime_generation;
    collected.info.loading_unit_id = entry_image->loading_unit_id;
    functions->push_back(collected);
    if (out_stage != nullptr) *out_stage = "complete";
    return true;
}

bool CollectFunctionsInClass(
    const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile, uint64_t heap_base,
    uint64_t tagged_class, uint32_t owner_class_id, bool is_top_level,
    const DartPlantFlutterSnapshotInfo& root_snapshot,
    std::span<const LiveVmInstructionImage> images, LiveLibraryUriCache* library_uri_cache,
    std::unordered_set<uint64_t>* seen_functions, std::vector<CollectedLiveFunction>* functions,
    uint32_t* skipped_function_count, uint32_t* function_rejection_logs, const char** out_stage) {
    if (out_stage != nullptr) *out_stage = "class-cid";
    uint32_t class_object_cid = 0;
    if (library_uri_cache == nullptr || seen_functions == nullptr || functions == nullptr ||
        skipped_function_count == nullptr || function_rejection_logs == nullptr ||
        owner_class_id == 0 || !ReadCid(reader, profile, tagged_class, &class_object_cid) ||
        class_object_cid != profile.cid_class) {
        return FailClassCollection(out_stage, "class-cid");
    }

    uint64_t library = 0;
    uint64_t class_functions = 0;
    // tagged_class was proved above, so do not make ReadClassLibrary repeat
    // the Class CID check. The observation-local URI cache performs the
    // Library CID proof exactly once per unique Library below.
    if (!ReadCompressedObject(reader, Untag(profile, tagged_class), profile,
                              profile.class_library_offset, heap_base, &library) ||
        !ReadCompressedObject(reader, Untag(profile, tagged_class), profile,
                              profile.class_functions_offset, heap_base, &class_functions)) {
        return FailClassCollection(out_stage, "class-roots");
    }

    char library_uri[DARTPLANT_LIVE_VM_LIBRARY_URI_MAX] = {};
    char class_name[DARTPLANT_LIVE_VM_CLASS_NAME_MAX] = {};
    if (!ReadCachedLibraryUri(reader, profile, heap_base, library, library_uri_cache, library_uri,
                              sizeof(library_uri))) {
        return FailClassCollection(out_stage, "library-uri");
    }
    if (is_top_level) {
        std::snprintf(class_name, sizeof(class_name), "%s", "Global");
    } else {
        uint64_t class_name_object = 0;
        if (!ReadCompressedObject(reader, Untag(profile, tagged_class), profile,
                                  profile.class_name_offset, heap_base, &class_name_object) ||
            !ReadDartString(reader, profile, class_name_object, class_name, sizeof(class_name))) {
            return FailClassCollection(out_stage, "class-name");
        }
    }

    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr) return FailClassCollection(out_stage, "functions-layout");
    if (!VisitArrayRawElements(
            reader, profile, class_functions, kMaxClassFunctions,
            [&](uint64_t index, uint32_t compressed) {
                if ((compressed & raw->smi_tag_mask) != raw->heap_object_tag) {
                    ++*skipped_function_count;
                    return ArrayVisitDecision::kContinue;
                }
                const uint64_t function = DecompressObject(heap_base, compressed);
                if (!RequireCid(reader, profile, function, profile.cid_function)) {
                    ++*skipped_function_count;
                    return ArrayVisitDecision::kContinue;
                }
                if (seen_functions->contains(function)) return ArrayVisitDecision::kContinue;

                const uintptr_t function_address = Untag(profile, function);
                uint32_t kind_tag = 0;
                if (!reader.Read(function_address + profile.function_kind_tag_offset, &kind_tag)) {
                    ++*skipped_function_count;
                    return ArrayVisitDecision::kContinue;
                }
                uint32_t kind = 0;
                if (!DecodeFunctionKind(profile, kind_tag, &kind)) {
                    ++*skipped_function_count;
                    return ArrayVisitDecision::kContinue;
                }
                const char* function_stage = "unknown";
                if (!CollectProvenLiveFunction(
                        reader, profile, heap_base, function, kind, tagged_class, library,
                        is_top_level, owner_class_id, static_cast<uint32_t>(index), library_uri,
                        class_name, root_snapshot, images, functions, &function_stage)) {
                    ++*skipped_function_count;
                    if (*function_rejection_logs < 20) {
                        ++*function_rejection_logs;
                        LogLiveIndex(
                            "function rejected class=0x%llx function=0x%llx kind=%u stage=%s "
                            "function_kind_off=0x%x code_size_off=0x%x",
                            static_cast<unsigned long long>(tagged_class),
                            static_cast<unsigned long long>(function), kind, function_stage,
                            profile.function_kind_tag_offset,
                            profile.code_instructions_length_offset);
                    }
                } else {
                    seen_functions->insert(function);
                }
                return ArrayVisitDecision::kContinue;
            })) {
        return FailClassCollection(out_stage, "functions-payload");
    }
    if (out_stage != nullptr) *out_stage = "complete";
    return true;
}

bool ResolveFunctionOwnerSlot(const ProcessMemoryReader& reader,
                              const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                              uint64_t owner_class, uint64_t function,
                              const std::unordered_map<uint64_t, uint32_t>& class_ids_by_object,
                              uint32_t* out_class_id, uint32_t* out_function_index) {
    if (out_class_id == nullptr || out_function_index == nullptr) return false;
    *out_class_id = 0;
    *out_function_index = UINT32_MAX;

    const auto owner_id = class_ids_by_object.find(owner_class);
    if (owner_id == class_ids_by_object.end() || owner_id->second == 0) return false;

    uint32_t class_object_cid = 0;
    uint64_t class_functions = 0;
    if (!ReadCid(reader, profile, owner_class, &class_object_cid) ||
        class_object_cid != profile.cid_class ||
        !ReadCompressedObject(reader, Untag(profile, owner_class), profile,
                              profile.class_functions_offset, heap_base, &class_functions)) {
        return false;
    }
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr) return false;
    bool found = false;
    const bool visited =
        VisitArrayRawElements(reader, profile, class_functions, kMaxClassFunctions,
                              [&](uint64_t index, uint32_t compressed) {
                                  if (index > UINT32_MAX) return ArrayVisitDecision::kFail;
                                  if ((compressed & raw->smi_tag_mask) != raw->heap_object_tag ||
                                      DecompressObject(heap_base, compressed) != function) {
                                      return ArrayVisitDecision::kContinue;
                                  }
                                  *out_class_id = owner_id->second;
                                  *out_function_index = static_cast<uint32_t>(index);
                                  found = true;
                                  return ArrayVisitDecision::kStop;
                              });
    return visited && found;
}

bool CollectDeferredFunction(
    const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile, uint64_t heap_base,
    uint64_t function, uint64_t expected_code, const LiveVmInstructionImage& target_image,
    const DartPlantFlutterSnapshotInfo& root_snapshot,
    std::span<const LiveVmInstructionImage> images,
    const std::unordered_map<uint64_t, uint32_t>& class_ids_by_object, uint32_t fallback_owner_id,
    uint32_t fallback_owner_index, LiveLibraryUriCache* library_uri_cache,
    std::unordered_set<uint64_t>* seen_functions, std::vector<CollectedLiveFunction>* functions,
    const char** out_stage) {
    if (out_stage != nullptr) *out_stage = "function-cid";
    if (library_uri_cache == nullptr || seen_functions == nullptr || functions == nullptr ||
        !RequireCid(reader, profile, function, profile.cid_function)) {
        return false;
    }
    if (seen_functions->contains(function)) {
        const auto found = std::find_if(functions->begin(), functions->end(),
                                        [function](const CollectedLiveFunction& existing) {
                                            return existing.info.function == function;
                                        });
        if (found == functions->end() || found->info.code != expected_code ||
            found->info.runtime_image_id != target_image.runtime_image_id ||
            found->info.loading_unit_id != target_image.loading_unit_id) {
            return FailFunctionCollection(out_stage, "duplicate-image-conflict");
        }
        if (out_stage != nullptr) *out_stage = "already-covered";
        return true;
    }

    const uintptr_t function_address = Untag(profile, function);
    uint32_t kind_tag = 0;
    uint32_t kind = 0;
    uint64_t owner_class = 0;
    if (!reader.Read(function_address + profile.function_kind_tag_offset, &kind_tag) ||
        !DecodeFunctionKind(profile, kind_tag, &kind) ||
        !ReadCompressedObject(reader, function_address, profile, profile.function_owner_offset,
                              heap_base, &owner_class) ||
        !RequireCid(reader, profile, owner_class, profile.cid_class)) {
        return FailFunctionCollection(out_stage, "function-owner");
    }

    uint64_t library = 0;
    uint64_t top_level_class = 0;
    char library_uri[DARTPLANT_LIVE_VM_LIBRARY_URI_MAX] = {};
    // owner_class was proved immediately above. Validate/cache the Library
    // once instead of revalidating both Class and Library on every Function.
    if (!ReadCompressedObject(reader, Untag(profile, owner_class), profile,
                              profile.class_library_offset, heap_base, &library) ||
        !ReadCachedLibraryUri(reader, profile, heap_base, library, library_uri_cache, library_uri,
                              sizeof(library_uri)) ||
        !ReadCompressedObject(reader, Untag(profile, library), profile,
                              profile.library_toplevel_class_offset, heap_base, &top_level_class)) {
        return FailFunctionCollection(out_stage, "function-library");
    }
    const bool is_top_level = owner_class == top_level_class;
    uint32_t owner_class_id = 0;
    uint32_t owner_function_index = UINT32_MAX;
    // Prefer the semantic Class.functions slot whenever Dart publishes one.
    // Some deferred implicit/synthetic Functions are Code.owner values but do
    // not occur in Class.functions. For those, the source-verified
    // InstructionsTable.code_objects[index] relation is the durable slot:
    // EndInstructions() appends one table per loaded unit and SetCodeAt()
    // fills the fixed index while deserializing that image.
    if (!ResolveFunctionOwnerSlot(reader, profile, heap_base, owner_class, function,
                                  class_ids_by_object, &owner_class_id, &owner_function_index)) {
        owner_class_id = fallback_owner_id;
        owner_function_index = fallback_owner_index;
    }
    char class_name[DARTPLANT_LIVE_VM_CLASS_NAME_MAX] = {};
    if (is_top_level) {
        std::snprintf(class_name, sizeof(class_name), "%s", "Global");
    } else {
        uint64_t class_name_object = 0;
        if (!ReadCompressedObject(reader, Untag(profile, owner_class), profile,
                                  profile.class_name_offset, heap_base, &class_name_object) ||
            !ReadDartString(reader, profile, class_name_object, class_name, sizeof(class_name))) {
            return FailFunctionCollection(out_stage, "class-name");
        }
    }

    const size_t before = functions->size();
    if (!CollectProvenLiveFunction(reader, profile, heap_base, function, kind, owner_class, library,
                                   is_top_level, owner_class_id, owner_function_index, library_uri,
                                   class_name, root_snapshot, images, functions, out_stage)) {
        return false;
    }
    if (functions->size() != before + 1 ||
        functions->back().info.runtime_image_id != target_image.runtime_image_id ||
        functions->back().info.loading_unit_id != target_image.loading_unit_id) {
        functions->resize(before);
        return FailFunctionCollection(out_stage, "deferred-image");
    }
    seen_functions->insert(function);
    return true;
}

bool CollectDeferredLoadingUnitFunctions(
    const ProcessMemoryReader& reader, const RuntimeProfileRecord& deferred_profile_record,
    const RuntimeProfileRecord& live_index_profile_record, const DartPlantLiveVmContext& context,
    const DartPlantFlutterSnapshotInfo& root_snapshot,
    std::span<const LiveVmInstructionImage> images,
    std::span<const uint64_t> target_runtime_image_ids,
    const std::unordered_map<uint64_t, uint32_t>& class_ids_by_object,
    std::unordered_set<uint64_t>* seen_functions, std::vector<CollectedLiveFunction>* functions,
    uint32_t* skipped_function_count) {
    if (images.size() <= 1) return true;
    if (seen_functions == nullptr || functions == nullptr || skipped_function_count == nullptr) {
        return false;
    }
    LiveLibraryUriCache library_uri_cache;
    const auto& profile = deferred_profile_record.live_vm;
    const auto& loading_unit = deferred_profile_record.loading_unit;
    if (profile.object_store_loading_units_offset == 0 || loading_unit.cid == 0 ||
        loading_unit.base_objects_offset == 0) {
        LogLiveIndex("deferred traversal unavailable profile=%s", profile.name);
        return false;
    }

    uint64_t loading_units = 0;
    if (!reader.Read(static_cast<uintptr_t>(context.object_store) +
                         profile.object_store_loading_units_offset,
                     &loading_units) ||
        !RequireCid(reader, profile, loading_units, profile.cid_array)) {
        LogLiveIndex("deferred loading_units root rejected raw=0x%llx",
                     static_cast<unsigned long long>(loading_units));
        return false;
    }
    uint64_t loading_unit_count = 0;
    if (!ReadArrayLength(reader, profile, loading_units, &loading_unit_count) ||
        loading_unit_count > kMaxClassFunctions) {
        LogLiveIndex("deferred loading_units length rejected count=%llu",
                     static_cast<unsigned long long>(loading_unit_count));
        return false;
    }

    for (const auto& image : images) {
        if (image.loading_unit_id <= 1) continue;
        if (!target_runtime_image_ids.empty() &&
            std::find(target_runtime_image_ids.begin(), target_runtime_image_ids.end(),
                      image.runtime_image_id) == target_runtime_image_ids.end()) {
            continue;
        }
        if (image.loading_unit_id >= loading_unit_count) {
            LogLiveIndex("deferred loading unit missing id=%u count=%llu", image.loading_unit_id,
                         static_cast<unsigned long long>(loading_unit_count));
            return false;
        }
        uint64_t unit = 0;
        uint64_t base_objects = 0;
        if (!ReadArrayElement(reader, profile, context.heap_base, loading_units,
                              image.loading_unit_id, &unit) ||
            !RequireCid(reader, profile, unit, loading_unit.cid) ||
            !ReadCompressedObject(reader, Untag(profile, unit), profile,
                                  loading_unit.base_objects_offset, context.heap_base,
                                  &base_objects)) {
            LogLiveIndex("deferred LoadingUnit rejected id=%u raw=0x%llx", image.loading_unit_id,
                         static_cast<unsigned long long>(unit));
            return false;
        }

        // A deferred ELF is process-global, while LoadingUnit load state is
        // IsolateGroup-local. Multiple FlutterEngine instances can therefore
        // see the same mapped secondary image even though this particular
        // group has not completed Dart_DeferredLoadComplete() for it yet. Dart
        // represents that state with LoadingUnit.base_objects == null. This is
        // not a malformed VM graph and must not make the root live index fail;
        // keep the physical RuntimeImage unbound (live_entry_count == 0) until
        // a later bootstrap observes the unit loaded in this group.
        uint64_t canonical_null = 0;
        if (profile.thread_object_null_offset == 0 || context.thread == 0 ||
            !reader.Read(static_cast<uintptr_t>(context.thread) + profile.thread_object_null_offset,
                         &canonical_null)) {
            LogLiveIndex("deferred canonical null unavailable id=%u", image.loading_unit_id);
            return false;
        }
        if (base_objects == canonical_null) {
            LogLiveIndex("deferred unit not loaded in current IsolateGroup id=%u",
                         image.loading_unit_id);
            continue;
        }

        // In bare-instructions AOT, Dart publishes one InstructionsTable for
        // the root image and appends one table for every deserialized deferred
        // image. Its code_objects array is already the exact VM-maintained Code
        // set for that instruction image. Prefer it over classifying every
        // LoadingUnit.base_objects slot by CID; retain the base-object walk as
        // a conservative fallback if the table cannot be proven uniquely.
        const auto& instructions_table = deferred_profile_record.instructions_table;
        bool used_instructions_table = false;
        if (instructions_table.object_store_offset != 0 && instructions_table.cid != 0 &&
            instructions_table.code_objects_offset != 0) {
            uint64_t tables = 0;
            if (reader.Read(static_cast<uintptr_t>(context.object_store) +
                                instructions_table.object_store_offset,
                            &tables) &&
                RequireCid(reader, profile, tables, profile.cid_growable_object_array)) {
                const uintptr_t growable = Untag(profile, tables);
                uint64_t table_count = 0;
                uint64_t table_data = 0;
                if (ReadPositiveCompressedSmi(
                        reader, growable + profile.growable_object_array_length_offset, profile,
                        &table_count) &&
                    table_count <= kMaxClassFunctions &&
                    ReadCompressedObject(reader, growable, profile,
                                         profile.growable_object_array_data_offset,
                                         context.heap_base, &table_data)) {
                    uint64_t matching_table = 0;
                    uint32_t matching_table_count = 0;
                    const uint64_t image_start = image.snapshot.isolate_instructions_runtime;
                    const uint64_t image_end =
                        image_start + image.snapshot.isolate_instructions_size;
                    if (image_end >= image_start) {
                        for (uint64_t table_index = 0; table_index < table_count; ++table_index) {
                            uint64_t table = 0;
                            if (!ReadArrayElement(reader, profile, context.heap_base, table_data,
                                                  table_index, &table) ||
                                !RequireCid(reader, profile, table, instructions_table.cid)) {
                                continue;
                            }
                            const uintptr_t table_address = Untag(profile, table);
                            uint64_t start_pc = 0;
                            uint64_t end_pc = 0;
                            if (!reader.Read(table_address + instructions_table.start_pc_offset,
                                             &start_pc) ||
                                !reader.Read(table_address + instructions_table.end_pc_offset,
                                             &end_pc) ||
                                start_pc >= end_pc || start_pc < image_start ||
                                end_pc > image_end) {
                                continue;
                            }
                            matching_table = table;
                            ++matching_table_count;
                        }
                    }

                    if (matching_table_count == 1) {
                        const uintptr_t table_address = Untag(profile, matching_table);
                        const auto table_owner_id =
                            StableInstructionsTableOwnerId(image.loading_unit_id);
                        uint64_t code_objects_array = 0;
                        uint64_t table_length = 0;
                        uint64_t code_object_count = 0;
                        if (table_owner_id.has_value() &&
                            reader.Read(table_address + instructions_table.code_objects_offset,
                                        &code_objects_array) &&
                            RequireCid(reader, profile, code_objects_array, profile.cid_array) &&
                            reader.Read(table_address + instructions_table.length_offset,
                                        &table_length) &&
                            table_length <= kMaxClassFunctions &&
                            ReadArrayLength(reader, profile, code_objects_array,
                                            &code_object_count) &&
                            code_object_count == table_length) {
                            uint32_t accepted = 0;
                            uint32_t code_objects = 0;
                            bool table_valid = true;
                            for (uint64_t code_index = 0; code_index < code_object_count;
                                 ++code_index) {
                                uint64_t code = 0;
                                if (!ReadArrayElement(reader, profile, context.heap_base,
                                                      code_objects_array, code_index, &code) ||
                                    !RequireCid(reader, profile, code, profile.cid_code)) {
                                    table_valid = false;
                                    break;
                                }
                                ++code_objects;
                                uint64_t entry = 0;
                                uint64_t entry_va = 0;
                                if (!reader.Read(
                                        Untag(profile, code) + profile.code_entry_point_offset,
                                        &entry) ||
                                    ResolveRuntimeInstructionImage(images, entry, 4, &entry_va) !=
                                        &image) {
                                    table_valid = false;
                                    break;
                                }
                                uint64_t function = 0;
                                if (!reader.Read(Untag(profile, code) + profile.code_owner_offset,
                                                 &function) ||
                                    !RequireCid(reader, profile, function, profile.cid_function)) {
                                    ++*skipped_function_count;
                                    continue;
                                }
                                const char* stage = "unknown";
                                if (CollectDeferredFunction(
                                        reader, live_index_profile_record.live_vm,
                                        context.heap_base, function, code, image, root_snapshot,
                                        images, class_ids_by_object, *table_owner_id,
                                        static_cast<uint32_t>(code_index), &library_uri_cache,
                                        seen_functions, functions, &stage)) {
                                    ++accepted;
                                } else {
                                    ++*skipped_function_count;
                                    if (*skipped_function_count <= 20) {
                                        LogLiveIndex(
                                            "deferred function rejected unit=%u index=%llu "
                                            "code=0x%llx function=0x%llx stage=%s",
                                            image.loading_unit_id,
                                            static_cast<unsigned long long>(code_index),
                                            static_cast<unsigned long long>(code),
                                            static_cast<unsigned long long>(function), stage);
                                    }
                                }
                            }
                            if (table_valid) {
                                LogLiveIndex(
                                    "deferred unit id=%u instructions_table_codes=%u functions=%u",
                                    image.loading_unit_id, code_objects, accepted);
                                used_instructions_table = true;
                            }
                        }
                    }
                }
            }
        }
        if (used_instructions_table) continue;

        uint32_t base_objects_cid = 0;
        uint64_t base_object_count = 0;
        if (!ReadCid(reader, profile, base_objects, &base_objects_cid) ||
            (base_objects_cid != profile.cid_array &&
             base_objects_cid != profile.cid_immutable_array) ||
            !ReadArrayLength(reader, profile, base_objects, &base_object_count) ||
            base_object_count > kMaxClassFunctions) {
            LogLiveIndex("deferred base_objects rejected id=%u raw=0x%llx count=%llu",
                         image.loading_unit_id, static_cast<unsigned long long>(base_objects),
                         static_cast<unsigned long long>(base_object_count));
            return false;
        }

        const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
        if (raw == nullptr || raw->compressed_word_size != sizeof(uint32_t) ||
            base_object_count > (std::numeric_limits<size_t>::max() / raw->compressed_word_size)) {
            LogLiveIndex("deferred base_objects payload layout rejected id=%u",
                         image.loading_unit_id);
            return false;
        }

        // base_objects has already been proven to be one bounded Dart Array.
        // Do not call ReadArrayElement() for every slot here: that helper
        // intentionally revalidates the same Array CID and length on each
        // access, which turns a 100k-slot loading unit into 300k redundant
        // observation reads. Read the contiguous compressed payload in small
        // fixed chunks, then keep the per-object CID/Code-owner validation.
        constexpr size_t kBaseObjectChunkSlots = 1024;
        std::array<uint32_t, kBaseObjectChunkSlots> compressed_slots{};
        const uintptr_t base_elements =
            Untag(profile, base_objects) + profile.array_elements_offset;

        uint32_t accepted = 0;
        uint32_t code_objects = 0;
        for (uint64_t chunk_start = 0; chunk_start < base_object_count;
             chunk_start += kBaseObjectChunkSlots) {
            const size_t chunk_count = static_cast<size_t>(
                std::min<uint64_t>(kBaseObjectChunkSlots, base_object_count - chunk_start));
            const uintptr_t chunk_address = base_elements + chunk_start * raw->compressed_word_size;
            if (!reader.ReadBytes(chunk_address, compressed_slots.data(),
                                  chunk_count * raw->compressed_word_size)) {
                LogLiveIndex("deferred base_objects payload read failed id=%u index=%llu",
                             image.loading_unit_id, static_cast<unsigned long long>(chunk_start));
                return false;
            }
            for (size_t offset = 0; offset < chunk_count; ++offset) {
                const uint32_t compressed = compressed_slots[offset];
                if ((compressed & raw->smi_tag_mask) != raw->heap_object_tag) continue;
                const uint64_t index = chunk_start + offset;
                const uint64_t code = DecompressObject(context.heap_base, compressed);
                uint32_t cid = 0;
                if (!ReadCid(reader, profile, code, &cid) || cid != profile.cid_code) continue;
                ++code_objects;
                uint64_t entry = 0;
                uint64_t entry_va = 0;
                if (!reader.Read(Untag(profile, code) + profile.code_entry_point_offset, &entry) ||
                    ResolveRuntimeInstructionImage(images, entry, 4, &entry_va) != &image) {
                    continue;
                }
                uint64_t function = 0;
                if (!reader.Read(Untag(profile, code) + profile.code_owner_offset, &function) ||
                    !RequireCid(reader, profile, function, profile.cid_function)) {
                    ++*skipped_function_count;
                    continue;
                }
                const char* stage = "unknown";
                if (CollectDeferredFunction(
                        reader, live_index_profile_record.live_vm, context.heap_base, function,
                        code, image, root_snapshot, images, class_ids_by_object, 0, UINT32_MAX,
                        &library_uri_cache, seen_functions, functions, &stage)) {
                    ++accepted;
                } else {
                    ++*skipped_function_count;
                    if (*skipped_function_count <= 20) {
                        LogLiveIndex(
                            "deferred function rejected unit=%u index=%llu code=0x%llx "
                            "function=0x%llx stage=%s",
                            image.loading_unit_id, static_cast<unsigned long long>(index),
                            static_cast<unsigned long long>(code),
                            static_cast<unsigned long long>(function), stage);
                    }
                }
            }
        }
        LogLiveIndex("deferred unit id=%u base_objects=%llu code_objects=%u functions=%u",
                     image.loading_unit_id, static_cast<unsigned long long>(base_object_count),
                     code_objects, accepted);
        // A valid secondary image need not contribute a new Function here.
        // Retained Functions may already have been recovered through
        // Class.functions/Library.toplevel_class; PRODUCT-dropped logical
        // aliases remain the exact artifact bundle's responsibility. Physical
        // image provenance and live semantic coverage are deliberately
        // independent, so zero additional Functions is not malformed.
    }
    return true;
}

bool CollectAllLiveFunctions(const ProcessMemoryReader& reader,
                             const RuntimeProfileRecord& profile_record,
                             const RuntimeProfileRecord* deferred_profile_record,
                             const DartPlantLiveVmContext& context,
                             const DartPlantFlutterSnapshotInfo& root_snapshot,
                             std::span<const LiveVmInstructionImage> images,
                             std::vector<CollectedLiveFunction>* functions,
                             DartPlantLiveVmFunctionIndexInfo* out_info) {
    if (functions == nullptr || out_info == nullptr) return false;
    const auto& profile = profile_record.live_vm;
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
        LogLiveIndex(
            "start rejected profile=%s class_table=0x%llx cached=0x%llx object_store=0x%llx "
            "num_cids=0x%llx max=0x%llx num_cids_off=0x%x",
            profile.name == nullptr ? "" : profile.name,
            static_cast<unsigned long long>(context.class_table),
            static_cast<unsigned long long>(context.cached_class_table_table),
            static_cast<unsigned long long>(context.object_store),
            static_cast<unsigned long long>(num_cids), static_cast<unsigned long long>(max_cids),
            profile.class_table_num_cids_offset);
        return false;
    }
    LogLiveIndex(
        "start profile=%s heap=0x%llx class_table=0x%llx cached=0x%llx object_store=0x%llx "
        "num_cids=%llu class_offsets=name:0x%x/functions:0x%x/library:0x%x",
        profile.name == nullptr ? "" : profile.name,
        static_cast<unsigned long long>(context.heap_base),
        static_cast<unsigned long long>(context.class_table),
        static_cast<unsigned long long>(context.cached_class_table_table),
        static_cast<unsigned long long>(context.object_store),
        static_cast<unsigned long long>(num_cids), profile.class_name_offset,
        profile.class_functions_offset, profile.class_library_offset);

    uint32_t nonzero_class_slots = 0;
    uint32_t accepted_classes = 0;
    uint32_t rejected_classes = 0;
    uint32_t rejection_logs = 0;
    uint32_t function_rejection_logs = 0;
    std::unordered_map<uint64_t, uint32_t> class_ids_by_object;
    class_ids_by_object.reserve(static_cast<size_t>(num_cids));
    LiveLibraryUriCache library_uri_cache;
    for (uint64_t cid = 1; cid < num_cids; ++cid) {
        uint64_t tagged_class = 0;
        if (!reader.Read(
                static_cast<uintptr_t>(context.cached_class_table_table) + cid * sizeof(uint64_t),
                &tagged_class) ||
            tagged_class == 0) {
            continue;
        }
        if (cid > UINT32_MAX ||
            !class_ids_by_object.emplace(tagged_class, static_cast<uint32_t>(cid)).second) {
            LogLiveIndex("class-table owner identity is ambiguous cid=%llu tagged=0x%llx",
                         static_cast<unsigned long long>(cid),
                         static_cast<unsigned long long>(tagged_class));
            return false;
        }
        ++nonzero_class_slots;
        const size_t before = functions->size();
        const char* stage = "unknown";
        if (CollectFunctionsInClass(reader, profile, context.heap_base, tagged_class,
                                    static_cast<uint32_t>(cid), false, root_snapshot, images,
                                    &library_uri_cache, &seen_functions, functions, &skipped,
                                    &function_rejection_logs, &stage)) {
            ++accepted_classes;
            if (accepted_classes <= 6) {
                LogLiveIndex("class accepted cid=%llu tagged=0x%llx added=%llu total=%llu",
                             static_cast<unsigned long long>(cid),
                             static_cast<unsigned long long>(tagged_class),
                             static_cast<unsigned long long>(functions->size() - before),
                             static_cast<unsigned long long>(functions->size()));
            }
        } else {
            ++rejected_classes;
            if (rejection_logs < 12) {
                ++rejection_logs;
                LogLiveIndex("class rejected cid=%llu tagged=0x%llx stage=%s",
                             static_cast<unsigned long long>(cid),
                             static_cast<unsigned long long>(tagged_class), stage);
            }
        }
    }

    uint64_t libraries = 0;
    if (!reader.Read(
            static_cast<uintptr_t>(context.object_store) + profile.object_store_libraries_offset,
            &libraries) ||
        !RequireCid(reader, profile, libraries, profile.cid_growable_object_array)) {
        LogLiveIndex("libraries root rejected raw=0x%llx offset=0x%x expected_cid=%u",
                     static_cast<unsigned long long>(libraries),
                     profile.object_store_libraries_offset, profile.cid_growable_object_array);
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
        LogLiveIndex("libraries container rejected raw=0x%llx count=%llu data=0x%llx",
                     static_cast<unsigned long long>(libraries),
                     static_cast<unsigned long long>(library_count),
                     static_cast<unsigned long long>(data));
        return false;
    }
    uint32_t accepted_top_levels = 0;
    uint32_t rejected_top_levels = 0;
    const RawObjectLayout* raw = FindRawObjectLayout(profile.profile_version);
    if (raw == nullptr) return false;
    uint64_t library_data_length = 0;
    if (!VisitArrayRawElements(
            reader, profile, data, kMaxClassFunctions,
            [&](uint64_t index, uint32_t compressed) {
                if (index >= library_count) return ArrayVisitDecision::kStop;
                if ((compressed & raw->smi_tag_mask) != raw->heap_object_tag) {
                    return ArrayVisitDecision::kContinue;
                }
                const uint64_t library = DecompressObject(context.heap_base, compressed);
                if (!RequireCid(reader, profile, library, profile.cid_library)) {
                    return ArrayVisitDecision::kContinue;
                }
                uint64_t top_level_class = 0;
                if (!ReadCompressedObject(reader, Untag(profile, library), profile,
                                          profile.library_toplevel_class_offset, context.heap_base,
                                          &top_level_class)) {
                    return ArrayVisitDecision::kContinue;
                }
                const auto top_level_owner_id = StableTopLevelOwnerId(index);
                if (!top_level_owner_id.has_value()) {
                    LogLiveIndex("top-level stable owner id overflow index=%llu",
                                 static_cast<unsigned long long>(index));
                    return ArrayVisitDecision::kFail;
                }
                const auto [top_level_it, inserted] =
                    class_ids_by_object.emplace(top_level_class, *top_level_owner_id);
                if (!inserted && top_level_it->second != *top_level_owner_id) {
                    LogLiveIndex(
                        "top-level owner identity is ambiguous index=%llu class=0x%llx existing=0x%x",
                        static_cast<unsigned long long>(index),
                        static_cast<unsigned long long>(top_level_class), top_level_it->second);
                    return ArrayVisitDecision::kFail;
                }
                const size_t before = functions->size();
                const char* stage = "unknown";
                if (CollectFunctionsInClass(reader, profile, context.heap_base, top_level_class,
                                            *top_level_owner_id, true, root_snapshot, images,
                                            &library_uri_cache, &seen_functions, functions,
                                            &skipped, &function_rejection_logs, &stage)) {
                    ++accepted_top_levels;
                    if (accepted_top_levels <= 6) {
                        LogLiveIndex(
                            "top-level accepted index=%llu library=0x%llx class=0x%llx added=%llu total=%llu",
                            static_cast<unsigned long long>(index),
                            static_cast<unsigned long long>(library),
                            static_cast<unsigned long long>(top_level_class),
                            static_cast<unsigned long long>(functions->size() - before),
                            static_cast<unsigned long long>(functions->size()));
                    }
                } else {
                    ++rejected_top_levels;
                    if (rejected_top_levels <= 12) {
                        LogLiveIndex(
                            "top-level rejected index=%llu library=0x%llx class=0x%llx stage=%s",
                            static_cast<unsigned long long>(index),
                            static_cast<unsigned long long>(library),
                            static_cast<unsigned long long>(top_level_class), stage);
                    }
                }
                return ArrayVisitDecision::kContinue;
            },
            &library_data_length) ||
        library_data_length < library_count) {
        LogLiveIndex("libraries data payload rejected count=%llu data_length=%llu",
                     static_cast<unsigned long long>(library_count),
                     static_cast<unsigned long long>(library_data_length));
        return false;
    }

    const RuntimeProfileRecord& deferred_profile =
        deferred_profile_record == nullptr ? profile_record : *deferred_profile_record;
    if (!CollectDeferredLoadingUnitFunctions(reader, deferred_profile, profile_record, context,
                                             root_snapshot, images, {}, class_ids_by_object,
                                             &seen_functions, functions, &skipped)) {
        LogLiveIndex("deferred loading-unit traversal failed profile=%s", profile.name);
        return false;
    }

    // Keep the public aggregate index counters compatible with the original
    // Function-index contract: they describe default Function entries. Exact
    // per-entry-kind multiplicity is exposed on each FunctionInfo above.
    FinalizeCollectedLiveFunctionAliases(functions, skipped, out_info);
    LogLiveIndex(
        "summary profile=%s class_slots=%u classes_ok=%u classes_rejected=%u libraries=%llu "
        "top_levels_ok=%u top_levels_rejected=%u functions=%u code_targets=%u shared=%u skipped=%u",
        profile.name == nullptr ? "" : profile.name, nonzero_class_slots, accepted_classes,
        rejected_classes, static_cast<unsigned long long>(library_count), accepted_top_levels,
        rejected_top_levels, out_info->function_count, out_info->code_target_count,
        out_info->shared_code_target_count, out_info->skipped_function_count);
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

// Observation-local only. Every key below is a movable Dart heap pointer and
// therefore must never survive the exact live-heap observation receipt that
// made it stable. The cached values themselves are immutable semantic copies.
struct LiveSemanticReadCache {
    std::unordered_map<uint64_t, ParsedFunctionSignature> function_types;
    std::unordered_map<uint64_t, DartPlantDartTypeInfo> dart_types;
    std::unordered_map<uint64_t, std::string> dart_strings;
    uint64_t function_type_hits = 0;
    uint64_t dart_type_hits = 0;
    uint64_t dart_string_hits = 0;
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

bool DecodeDartTypeCached(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                          const FunctionTypeLayout& layout, uint64_t tagged_type,
                          LiveSemanticReadCache* cache, DartPlantDartTypeInfo* out_type) {
    if (cache == nullptr) {
        return DecodeDartType(reader, profile, layout, tagged_type, out_type);
    }
    const auto found = cache->dart_types.find(tagged_type);
    if (found != cache->dart_types.end()) {
        ++cache->dart_type_hits;
        *out_type = found->second;
        return true;
    }
    DartPlantDartTypeInfo decoded{};
    if (!DecodeDartType(reader, profile, layout, tagged_type, &decoded)) return false;
    cache->dart_types.emplace(tagged_type, decoded);
    *out_type = decoded;
    return true;
}

bool ReadDartStringCached(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                          uint64_t tagged_string, LiveSemanticReadCache* cache, char* output,
                          size_t capacity) {
    if (cache == nullptr) {
        return ReadDartString(reader, profile, tagged_string, output, capacity);
    }
    if (output == nullptr || capacity == 0) return false;
    const auto found = cache->dart_strings.find(tagged_string);
    if (found != cache->dart_strings.end()) {
        if (found->second.size() >= capacity) return false;
        ++cache->dart_string_hits;
        std::memcpy(output, found->second.c_str(), found->second.size() + 1);
        return true;
    }
    std::array<char, DARTPLANT_DART_PARAMETER_NAME_MAX> value{};
    if (!ReadDartString(reader, profile, tagged_string, value.data(), value.size())) {
        return false;
    }
    const auto [inserted, ok] = cache->dart_strings.emplace(tagged_string, value.data());
    if (!ok || inserted->second.size() >= capacity) return false;
    std::memcpy(output, inserted->second.c_str(), inserted->second.size() + 1);
    return true;
}

DartPlantStatus ParseRetainedFunctionSignature(const ProcessMemoryReader& reader,
                                               const DartPlantLiveVmProfile& profile,
                                               const FunctionTypeLayout& layout, uint64_t heap_base,
                                               uint64_t tagged_function,
                                               ParsedFunctionSignature* out_signature,
                                               LiveSemanticReadCache* cache = nullptr) {
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
    if (cache != nullptr) {
        const auto found = cache->function_types.find(tagged_signature);
        if (found != cache->function_types.end()) {
            ++cache->function_type_hits;
            *out_signature = found->second;
            return DARTPLANT_OK;
        }
    }

    const uintptr_t signature_address = Untag(profile, tagged_signature);
    uint32_t packed_counts = 0;
    uint16_t packed_type_counts = 0;
    uint64_t result_type = 0;
    ObjectFieldWindow signature_fields;
    const std::array signature_field_layout = {
        std::pair{layout.packed_parameter_counts_offset, sizeof(uint32_t)},
        std::pair{layout.packed_type_parameter_counts_offset, sizeof(uint16_t)},
        std::pair{layout.result_type_offset, sizeof(uint32_t)},
        std::pair{layout.parameter_types_offset, sizeof(uint32_t)},
        std::pair{layout.named_parameter_names_offset, sizeof(uint32_t)},
    };
    if (!ReadObjectFieldWindow(reader, signature_address, signature_field_layout,
                               &signature_fields) ||
        !signature_fields.Load(layout.packed_parameter_counts_offset, &packed_counts) ||
        !signature_fields.Load(layout.packed_type_parameter_counts_offset, &packed_type_counts) ||
        !DecodeCompressedObjectField(signature_fields, layout.result_type_offset, profile,
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
    if (!DecodeDartTypeCached(reader, profile, layout, result_type, cache, &parsed.result_type)) {
        return FailProbe("live VM FunctionType result type is invalid");
    }

    if (parsed.parameter_count != 0) {
        if (!DecodeCompressedObjectField(signature_fields, layout.parameter_types_offset, profile,
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
        if (!DecodeCompressedObjectField(signature_fields, layout.named_parameter_names_offset,
                                         profile, heap_base, &parsed.named_parameter_names)) {
            return FailProbe("live VM FunctionType named_parameter_names is unreadable");
        }
        uint64_t named_slot_count = 0;
        const uint64_t flag_slot_count =
            (parsed.optional_parameter_count + kNamedParameterFlagsPerSmi - 1) /
            kNamedParameterFlagsPerSmi;
        const uint64_t maximum_named_slot_count =
            static_cast<uint64_t>(parsed.optional_parameter_count) + flag_slot_count;
        if (!ReadArrayLength(reader, profile, parsed.named_parameter_names, &named_slot_count) ||
            named_slot_count < parsed.optional_parameter_count ||
            named_slot_count > maximum_named_slot_count) {
            return FailProbe("live VM FunctionType named_parameter_names is inconsistent");
        }
    }

    if (cache != nullptr) cache->function_types.emplace(tagged_signature, parsed);
    *out_signature = parsed;
    return DARTPLANT_OK;
}

DartPlantStatus ParseRetainedFunctionSignatureWithRetry(const ProcessMemoryReader& reader,
                                                        const DartPlantLiveVmProfile& profile,
                                                        const FunctionTypeLayout& layout,
                                                        uint64_t heap_base,
                                                        uint64_t tagged_function,
                                                        ParsedFunctionSignature* out_signature) {
    DartPlantStatus status = DARTPLANT_PROFILE_MISMATCH;
    const uint32_t attempts = reader.volatile_reads() ? kVolatileHeapReadAttempts : 1;
    for (uint32_t attempt = 0; attempt < attempts; ++attempt) {
        ParsedFunctionSignature parsed{};
        status = ParseRetainedFunctionSignature(reader, profile, layout, heap_base, tagged_function,
                                                &parsed);
        if (status == DARTPLANT_OK) {
            *out_signature = parsed;
            return DARTPLANT_OK;
        }
        if (status != DARTPLANT_PROFILE_MISMATCH) return status;
        if (reader.volatile_reads()) std::this_thread::yield();
    }
    return status;
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
    if (context.profile_version != profile.profile_version) {
        SetLastError("live VM Bool root profile does not match the captured context");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const RuntimeProfileRecord* record = FindRuntimeProfileByVersion(profile.profile_version);
    if (record == nullptr || record->live_vm.name == nullptr || profile.name == nullptr ||
        std::strcmp(record->live_vm.name, profile.name) != 0) {
        SetLastError("live VM Bool root profile is not a known source row");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    return ProbeLiveVmCanonicalBoolRootsForCandidate(context, *record, out_true, out_false);
}

DartPlantStatus ProbeLiveVmCanonicalBoolRootsForCandidate(const DartPlantLiveVmContext& context,
                                                          const RuntimeProfileRecord& candidate,
                                                          uint64_t* out_true, uint64_t* out_false) {
    const DartPlantLiveVmProfile& profile = candidate.live_vm;
    const CanonicalBoolLayout& layout = candidate.canonical_bool;
    if (out_true == nullptr || out_false == nullptr || context.thread == 0 || layout.cid == 0) {
        SetLastError("live VM Bool root arguments/profile are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    ProcessMemoryReader reader = ProcessMemoryReader::VolatileSafe();
    if (!reader.Refresh()) {
        SetLastError("failed to inspect process mappings for Dart Bool roots");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    uint64_t bool_true = 0;
    uint64_t bool_false = 0;
    if (!reader.Read(static_cast<uintptr_t>(context.thread) + layout.thread_true_offset,
                     &bool_true) ||
        !reader.Read(static_cast<uintptr_t>(context.thread) + layout.thread_false_offset,
                     &bool_false) ||
        bool_true == 0 || bool_false == 0 || bool_true == bool_false ||
        !RequireCid(reader, profile, bool_true, layout.cid) ||
        !RequireCid(reader, profile, bool_false, layout.cid)) {
        SetLastError("Dart canonical Bool roots failed CID validation");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    uint8_t true_value = 0;
    uint8_t false_value = 0xff;
    if (!reader.Read(Untag(profile, bool_true) + layout.value_offset, &true_value) ||
        !reader.Read(Untag(profile, bool_false) + layout.value_offset, &false_value) ||
        true_value != 1 || false_value != 0) {
        SetLastError("Dart canonical Bool roots failed value validation");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    *out_true = bool_true;
    *out_false = bool_false;
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus ProbeLiveVmRootProgramHashForCandidate(const DartPlantLiveVmContext& context,
                                                       const RuntimeProfileRecord& candidate,
                                                       uint32_t* out_program_hash) {
    if (out_program_hash == nullptr || context.object_store == 0 || context.heap_base == 0 ||
        candidate.live_vm.object_store_loading_units_offset == 0) {
        SetLastError("live VM deferred program-hash candidate arguments/profile are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    ProcessMemoryReader reader = ProcessMemoryReader::VolatileSafe();
    if (!reader.Refresh()) {
        SetLastError("cannot inspect process mappings for deferred program hash");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    uint64_t loading_units = 0;
    if (!reader.Read(static_cast<uintptr_t>(context.object_store) +
                         candidate.live_vm.object_store_loading_units_offset,
                     &loading_units) ||
        !RequireCid(reader, candidate.live_vm, loading_units, candidate.live_vm.cid_array)) {
        SetLastError("ObjectStore.loading_units is unavailable for deferred program proof");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const uintptr_t array = Untag(candidate.live_vm, loading_units);
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, array + candidate.live_vm.array_length_offset,
                                   candidate.live_vm, &length) ||
        length <= 1) {
        SetLastError("ObjectStore.loading_units has no deferred-program root entry");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    uint64_t program_hash = 0;
    if (!ReadPositiveCompressedSmi(reader, array + candidate.live_vm.array_elements_offset,
                                   candidate.live_vm, &program_hash) ||
        program_hash > UINT32_MAX) {
        SetLastError("ObjectStore.loading_units[0] is not a valid program-hash Smi");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    *out_program_hash = static_cast<uint32_t>(program_hash);
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus ProbeLiveVmDeferredLoadingUnitStatesForImages(
    const DartPlantLiveVmContext& context, std::span<const LiveVmInstructionImage> images,
    const RuntimeProfileRecord& deferred_profile, uint64_t canonical_null,
    DartPlantVmAdapter* observation_adapter, const void* observation_lease,
    std::vector<LiveVmDeferredLoadingUnitState>* out_states) {
    if (out_states == nullptr || context.object_store == 0 || context.heap_base == 0 ||
        context.thread == 0 || canonical_null == 0 ||
        deferred_profile.live_vm.object_store_loading_units_offset == 0 ||
        deferred_profile.loading_unit.cid == 0 ||
        deferred_profile.loading_unit.base_objects_offset == 0) {
        SetLastError("live VM deferred load-state probe arguments/profile are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    auto reader = ProcessMemoryReader::ObservationScopedDirect(observation_adapter, context.thread,
                                                               observation_lease);
    if (!reader.has_value()) {
        SetLastError(
            "live VM deferred load-state probe requires the current thread's exact "
            "moving-GC observation receipt");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (!reader->Refresh()) {
        SetLastError("cannot inspect process mappings for deferred load state");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    const auto& profile = deferred_profile.live_vm;
    uint64_t loading_units = 0;
    if (!reader->Read(static_cast<uintptr_t>(context.object_store) +
                          profile.object_store_loading_units_offset,
                      &loading_units) ||
        !RequireCid(*reader, profile, loading_units, profile.cid_array)) {
        SetLastError("ObjectStore.loading_units is unavailable for deferred load-state proof");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    uint64_t loading_unit_count = 0;
    if (!ReadArrayLength(*reader, profile, loading_units, &loading_unit_count) ||
        loading_unit_count > kMaxClassFunctions) {
        SetLastError("ObjectStore.loading_units length is invalid for deferred load-state proof");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    std::vector<LiveVmDeferredLoadingUnitState> states;
    states.reserve(images.size());
    for (const auto& image : images) {
        if (image.loading_unit_id <= 1) continue;
        if (image.runtime_image_id == 0 || image.loading_unit_id >= loading_unit_count) {
            SetLastError("deferred RuntimeImage has no matching live LoadingUnit");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        uint64_t unit = 0;
        uint64_t base_objects = 0;
        if (!ReadArrayElement(*reader, profile, context.heap_base, loading_units,
                              image.loading_unit_id, &unit) ||
            !RequireCid(*reader, profile, unit, deferred_profile.loading_unit.cid) ||
            !ReadCompressedObject(*reader, Untag(profile, unit), profile,
                                  deferred_profile.loading_unit.base_objects_offset,
                                  context.heap_base, &base_objects)) {
            SetLastError("deferred LoadingUnit state is unreadable");
            return DARTPLANT_PROFILE_MISMATCH;
        }

        const bool loaded = base_objects != canonical_null;
        if (loaded) {
            uint32_t base_objects_cid = 0;
            if (!ReadCid(*reader, profile, base_objects, &base_objects_cid) ||
                (base_objects_cid != profile.cid_array &&
                 base_objects_cid != profile.cid_immutable_array)) {
                SetLastError("loaded deferred LoadingUnit has invalid base_objects");
                return DARTPLANT_PROFILE_MISMATCH;
            }
        }
        states.push_back({
            .runtime_image_id = image.runtime_image_id,
            .loading_unit_id = image.loading_unit_id,
            .loaded = loaded,
        });
    }

    *out_states = std::move(states);
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus ReadLiveVmRootProgramHashForCurrentProfile(const DartPlantLiveVmContext& context,
                                                           const RuntimeProfileRecord& profile,
                                                           uint32_t* out_program_hash) {
    if (context.profile_version != profile.live_vm.profile_version) {
        SetLastError("live VM deferred program-hash profile does not match the captured context");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    return ProbeLiveVmRootProgramHashForCandidate(context, profile, out_program_hash);
}

DartPlantStatus ReadLiveVmFunctionSignatureForProfile(
    const DartPlantLiveVmContext& context, const RuntimeProfileRecord& profile, uint64_t function,
    DartPlantDartFunctionSignatureInfo* out_signature) {
    if (out_signature == nullptr ||
        out_signature->struct_size < sizeof(DartPlantDartFunctionSignatureInfo) ||
        context.heap_base == 0 || function == 0) {
        SetLastError("live VM FunctionType profile parser arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    ProcessMemoryReader reader = ProcessMemoryReader::VolatileSafe();
    if (!reader.Refresh()) {
        SetLastError("cannot inspect process mappings for FunctionType parsing");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    ParsedFunctionSignature parsed{};
    const DartPlantStatus status = ParseRetainedFunctionSignatureWithRetry(
        reader, profile.live_vm, profile.function_type, context.heap_base, function, &parsed);
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
    ClearLastError();
    return DARTPLANT_OK;
}

namespace {

DartPlantStatus DecodeLiveVmFunctionParameter(const ProcessMemoryReader& reader,
                                              const RuntimeProfileRecord& profile,
                                              uint64_t heap_base,
                                              const ParsedFunctionSignature& parsed, uint32_t index,
                                              DartPlantDartParameterInfo* out_parameter) {
    if (out_parameter == nullptr ||
        out_parameter->struct_size < sizeof(DartPlantDartParameterInfo)) {
        SetLastError("live VM FunctionType parameter output is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (index >= parsed.parameter_count) {
        SetLastError("FunctionType parameter index is out of range");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    uint64_t tagged_type = 0;
    if (!ReadArrayElement(reader, profile.live_vm, heap_base, parsed.parameter_types, index,
                          &tagged_type)) {
        return FailProbe("FunctionType parameter type is unreadable");
    }
    DartPlantDartParameterInfo parameter{};
    parameter.struct_size = sizeof(parameter);
    parameter.index = index;
    if (!DecodeDartType(reader, profile.live_vm, profile.function_type, tagged_type,
                        &parameter.type)) {
        return FailProbe("FunctionType parameter type is invalid");
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
        if (!ReadArrayElement(reader, profile.live_vm, heap_base, parsed.named_parameter_names,
                              named_index, &tagged_name) ||
            !ReadDartString(reader, profile.live_vm, tagged_name, parameter.name,
                            sizeof(parameter.name))) {
            return FailProbe("FunctionType named parameter name is invalid");
        }
        const uint32_t flag_index =
            parsed.optional_parameter_count + named_index / kNamedParameterFlagsPerSmi;
        uint64_t named_slot_count = 0;
        uint32_t raw_flags = 0;
        if (!ReadArrayLength(reader, profile.live_vm, parsed.named_parameter_names,
                             &named_slot_count)) {
            return FailProbe("FunctionType required-named flags are invalid");
        }
        uint32_t flags = 0;
        if (flag_index < named_slot_count) {
            if (!ReadArrayRawElement(reader, profile.live_vm, parsed.named_parameter_names,
                                     flag_index, &raw_flags) ||
                (raw_flags & profile.raw_object.smi_tag_mask) != profile.raw_object.smi_tag) {
                return FailProbe("FunctionType required-named flags are invalid");
            }
            flags = raw_flags >> profile.raw_object.smi_tag_shift;
        }
        parameter.is_required =
            (flags & (1U << (named_index % kNamedParameterFlagsPerSmi))) != 0 ? 1 : 0;
    }
    *out_parameter = parameter;
    return DARTPLANT_OK;
}

void CopyParsedFunctionSignature(const ParsedFunctionSignature& parsed,
                                 DartPlantDartFunctionSignatureInfo* out_signature) {
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
}

bool ReadArrayRawSnapshot(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                          uint64_t tagged_array, uint64_t max_length,
                          std::vector<uint32_t>* out_slots) {
    if (out_slots == nullptr) return false;
    std::vector<uint32_t> slots;
    uint64_t length = 0;
    const bool visited = VisitArrayRawElements(
        reader, profile, tagged_array, max_length,
        [&slots](uint64_t, uint32_t raw) {
            slots.push_back(raw);
            return ArrayVisitDecision::kContinue;
        },
        &length);
    if (!visited || slots.size() != length) return false;
    *out_slots = std::move(slots);
    return true;
}

DartPlantStatus DecodeLiveVmFunctionParametersSnapshot(
    const ProcessMemoryReader& reader, const RuntimeProfileRecord& profile, uint64_t heap_base,
    const ParsedFunctionSignature& parsed, LiveSemanticReadCache* cache,
    std::vector<DartPlantDartParameterInfo>* out_parameters) {
    if (out_parameters == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    const RawObjectLayout* raw = FindRawObjectLayout(profile.live_vm.profile_version);
    if (raw == nullptr || raw->compressed_word_size != sizeof(uint32_t)) {
        return FailProbe("FunctionType compressed-array layout is unavailable");
    }

    std::vector<uint32_t> parameter_slots;
    if (parsed.parameter_count != 0 &&
        (!ReadArrayRawSnapshot(reader, profile.live_vm, parsed.parameter_types,
                               parsed.parameter_count, &parameter_slots) ||
         parameter_slots.size() != parsed.parameter_count)) {
        return FailProbe("FunctionType parameter type array is inconsistent");
    }

    std::vector<uint32_t> named_slots;
    if (parsed.has_named_optional_parameters) {
        const uint64_t flag_slot_count = (static_cast<uint64_t>(parsed.optional_parameter_count) +
                                          kNamedParameterFlagsPerSmi - 1) /
                                         kNamedParameterFlagsPerSmi;
        const uint64_t maximum_named_slot_count =
            static_cast<uint64_t>(parsed.optional_parameter_count) + flag_slot_count;
        if (!ReadArrayRawSnapshot(reader, profile.live_vm, parsed.named_parameter_names,
                                  maximum_named_slot_count, &named_slots) ||
            named_slots.size() < parsed.optional_parameter_count) {
            return FailProbe("FunctionType named parameter array is inconsistent");
        }
    }

    std::vector<DartPlantDartParameterInfo> parameters(parsed.parameter_count);
    for (uint32_t index = 0; index < parsed.parameter_count; ++index) {
        const uint32_t raw_type = parameter_slots[index];
        if ((raw_type & raw->smi_tag_mask) != raw->heap_object_tag) {
            return FailProbe("FunctionType parameter type is unreadable");
        }
        const uint64_t tagged_type = DecompressObject(heap_base, raw_type);
        auto& parameter = parameters[index];
        parameter.struct_size = sizeof(parameter);
        parameter.index = index;
        if (!DecodeDartTypeCached(reader, profile.live_vm, profile.function_type, tagged_type,
                                  cache, &parameter.type)) {
            return FailProbe("FunctionType parameter type is invalid");
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
            if (named_index >= parsed.optional_parameter_count ||
                named_index >= named_slots.size()) {
                return FailProbe("FunctionType named parameter name is invalid");
            }
            const uint32_t raw_name = named_slots[named_index];
            if ((raw_name & raw->smi_tag_mask) != raw->heap_object_tag) {
                return FailProbe("FunctionType named parameter name is invalid");
            }
            const uint64_t tagged_name = DecompressObject(heap_base, raw_name);
            if (!ReadDartStringCached(reader, profile.live_vm, tagged_name, cache, parameter.name,
                                      sizeof(parameter.name))) {
                return FailProbe("FunctionType named parameter name is invalid");
            }

            const uint32_t flag_index =
                parsed.optional_parameter_count + named_index / kNamedParameterFlagsPerSmi;
            uint32_t flags = 0;
            if (flag_index < named_slots.size()) {
                const uint32_t raw_flags = named_slots[flag_index];
                if ((raw_flags & raw->smi_tag_mask) != raw->smi_tag) {
                    return FailProbe("FunctionType required-named flags are invalid");
                }
                flags = raw_flags >> raw->smi_tag_shift;
            }
            parameter.is_required =
                (flags & (1U << (named_index % kNamedParameterFlagsPerSmi))) != 0 ? 1 : 0;
        }
    }
    *out_parameters = std::move(parameters);
    return DARTPLANT_OK;
}

}  // namespace

DartPlantStatus ReadLiveVmFunctionParameterForProfile(const DartPlantLiveVmContext& context,
                                                      const RuntimeProfileRecord& profile,
                                                      uint64_t function, uint32_t index,
                                                      DartPlantDartParameterInfo* out_parameter) {
    if (out_parameter == nullptr ||
        out_parameter->struct_size < sizeof(DartPlantDartParameterInfo) || context.heap_base == 0 ||
        function == 0) {
        SetLastError("live VM FunctionType parameter profile parser arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    ProcessMemoryReader reader = ProcessMemoryReader::VolatileSafe();
    if (!reader.Refresh()) {
        SetLastError("cannot inspect process mappings for FunctionType parsing");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    ParsedFunctionSignature parsed{};
    DartPlantStatus status = ParseRetainedFunctionSignatureWithRetry(
        reader, profile.live_vm, profile.function_type, context.heap_base, function, &parsed);
    if (status != DARTPLANT_OK) return status;
    status = DecodeLiveVmFunctionParameter(reader, profile, context.heap_base, parsed, index,
                                           out_parameter);
    if (status != DARTPLANT_OK) return status;
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus ReadLiveVmFunctionSemanticsForProfile(
    const DartPlantLiveVmContext& context, const RuntimeProfileRecord& profile, uint64_t function,
    DartPlantDartFunctionSignatureInfo* out_signature,
    std::vector<DartPlantDartParameterInfo>* out_parameters) {
    if (out_signature == nullptr ||
        out_signature->struct_size < sizeof(DartPlantDartFunctionSignatureInfo) ||
        out_parameters == nullptr || context.heap_base == 0 || function == 0) {
        SetLastError("live VM FunctionType semantic snapshot arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    ProcessMemoryReader reader = ProcessMemoryReader::VolatileSafe();
    if (!reader.Refresh()) {
        SetLastError("cannot inspect process mappings for FunctionType semantic snapshot");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    ParsedFunctionSignature parsed{};
    DartPlantStatus status = ParseRetainedFunctionSignatureWithRetry(
        reader, profile.live_vm, profile.function_type, context.heap_base, function, &parsed);
    if (status != DARTPLANT_OK) return status;

    std::vector<DartPlantDartParameterInfo> parameters(parsed.parameter_count);
    for (uint32_t index = 0; index < parsed.parameter_count; ++index) {
        parameters[index].struct_size = sizeof(DartPlantDartParameterInfo);
        status = DecodeLiveVmFunctionParameter(reader, profile, context.heap_base, parsed, index,
                                               &parameters[index]);
        if (status != DARTPLANT_OK) return status;
    }
    CopyParsedFunctionSignature(parsed, out_signature);
    *out_parameters = std::move(parameters);
    ClearLastError();
    return DARTPLANT_OK;
}

namespace {

DartPlantStatus ReadLiveVmFunctionSemanticsWithReader(
    const ProcessMemoryReader& reader, const DartPlantLiveVmContext& context,
    const RuntimeProfileRecord& profile, uint64_t function,
    DartPlantDartFunctionSignatureInfo* out_signature,
    std::vector<DartPlantDartParameterInfo>* out_parameters, LiveSemanticReadCache* cache) {
    if (out_signature == nullptr ||
        out_signature->struct_size < sizeof(DartPlantDartFunctionSignatureInfo) ||
        out_parameters == nullptr || context.heap_base == 0 || function == 0) {
        SetLastError("live VM FunctionType semantic snapshot arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    ParsedFunctionSignature parsed{};
    DartPlantStatus status =
        reader.volatile_reads()
            ? ParseRetainedFunctionSignatureWithRetry(reader, profile.live_vm,
                                                      profile.function_type, context.heap_base,
                                                      function, &parsed)
            : ParseRetainedFunctionSignature(reader, profile.live_vm, profile.function_type,
                                             context.heap_base, function, &parsed, cache);
    if (status != DARTPLANT_OK) return status;
    std::vector<DartPlantDartParameterInfo> parameters;
    status = DecodeLiveVmFunctionParametersSnapshot(reader, profile, context.heap_base, parsed,
                                                    cache, &parameters);
    if (status != DARTPLANT_OK) return status;
    CopyParsedFunctionSignature(parsed, out_signature);
    *out_parameters = std::move(parameters);
    ClearLastError();
    return DARTPLANT_OK;
}

}  // namespace

LiveVmCandidateResolution ResolveLiveVmCandidateForArm64ContextInternal(
    const DartPlantFlutterSnapshotInfo& snapshot, const DartPlantArm64Context& context) {
    VmRuntimeFacts facts{};
    facts.snapshot_hash = snapshot.snapshot_hash == nullptr ? "" : snapshot.snapshot_hash;
    facts.snapshot_features =
        snapshot.snapshot_features == nullptr ? "" : snapshot.snapshot_features;
    facts.compressed_pointers = snapshot.compressed_pointers != 0;
    const auto profiles = ResolveRuntimeProfileCandidates(facts);
    vm_abi::AbiCandidateSet candidates{};
    candidates.profiles = profiles;
    std::vector<vm_abi::RootProof> roots;
    roots.reserve(profiles.size());
    std::vector<bool> compatible;
    compatible.reserve(profiles.size());
    for (const auto* profile : profiles) {
        vm_abi::RootProof root{};
        if (profile != nullptr && profile->live_vm.thr_register < 31 &&
            profile->live_vm.pp_register < 31 && profile->live_vm.heap_bits_register < 31 &&
            profile->live_vm.null_register < 31) {
            vm_abi::RootProofInput input{};
            input.profile = profile;
            input.thread = CanonicalNativePointer(context.x[profile->live_vm.thr_register]);
            input.require_dart_core = true;
            input.registers = {
                .available = true,
                .pp = context.x[profile->live_vm.pp_register],
                .heap_bits = context.x[profile->live_vm.heap_bits_register],
                .null_value = context.x[profile->live_vm.null_register],
            };
            root = vm_abi::ProveRuntimeRoots(input);
        }
        roots.push_back(root);
        compatible.push_back(root.passed);
    }
    const auto selection =
        vm_abi::SelectCapabilityAbiSet(candidates, vm_abi::kCapabilityRuntimeRoots, compatible);
    if (!selection.passed()) {
        SetLastError(selection.ambiguous()
                         ? "live VM runtime-root capability is ambiguous across candidate profiles"
                         : "live VM runtime-root capability rejected every candidate profile");
        return {};
    }
    const auto found = std::find(profiles.begin(), profiles.end(), selection.representative);
    if (found == profiles.end()) {
        SetLastError("live VM runtime-root capability selected an unknown profile");
        return {};
    }
    return {
        .profile = selection.representative,
        .probe = {.roots = roots[static_cast<size_t>(std::distance(profiles.begin(), found))]},
    };
}

DartPlantStatus ResolveLiveVmCandidateForArm64Context(const DartPlantFlutterSnapshotInfo& snapshot,
                                                      const DartPlantArm64Context& context,
                                                      LiveVmCandidateResolution* out_resolution) {
    if (out_resolution == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    *out_resolution = ResolveLiveVmCandidateForArm64ContextInternal(snapshot, context);
    if (out_resolution->profile == nullptr) return DARTPLANT_PROFILE_MISMATCH;
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus ResolveLiveVmCandidateForRegisters(const DartPlantFlutterSnapshotInfo& snapshot,
                                                   const DartPlantLiveVmArm64Registers& registers,
                                                   LiveVmCandidateResolution* out_resolution,
                                                   DartPlantLiveVmContext* out_context) {
    if (out_resolution == nullptr || out_context == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    DartPlantArm64Context context{};
    context.pc = registers.pc;
    context.sp = registers.sp;
    context.x[22] = registers.null_value;
    context.x[26] = registers.thr;
    context.x[27] = registers.pp;
    context.x[28] = registers.heap_bits;
    const DartPlantStatus status =
        ResolveLiveVmCandidateForArm64Context(snapshot, context, out_resolution);
    if (status != DARTPLANT_OK) return status;
    const auto& profile = *out_resolution->profile;
    const auto& roots = out_resolution->probe.roots;
    DartPlantLiveVmContext result{};
    result.struct_size = sizeof(result);
    result.profile_version = profile.live_vm.profile_version;
    result.profile_name = profile.live_vm.name;
    result.thread = CanonicalNativePointer(registers.thr);
    result.isolate = roots.isolate;
    result.isolate_group = roots.isolate_group;
    result.class_table = roots.class_table;
    result.cached_class_table_table = roots.cached_class_table_table;
    result.object_store = roots.object_store;
    result.heap_base = roots.heap_base;
    result.pp = registers.pp;
    result.global_object_pool = roots.global_object_pool;
    result.object_pool_length = roots.object_pool_length;
    *out_context = result;
    return DARTPLANT_OK;
}

DartPlantStatus VisitLiveVmFunctionsForProfile(const DartPlantLiveVmContext& context,
                                               const DartPlantFlutterSnapshotInfo& snapshot,
                                               const RuntimeProfileRecord& profile_record,
                                               DartPlantLiveVmFunctionVisitor visitor,
                                               void* user_data,
                                               DartPlantLiveVmFunctionIndexInfo* out_info) {
    LiveVmInstructionImage image{};
    image.runtime_image_id = 0;
    image.loading_unit_id = 1;
    image.snapshot = snapshot;
    const std::array<LiveVmInstructionImage, 1> images = {image};
    return VisitLiveVmFunctionsForImages(context, images, profile_record, nullptr, visitor,
                                         user_data, out_info);
}

DartPlantStatus VisitLiveVmFunctionsForImages(const DartPlantLiveVmContext& context,
                                              std::span<const LiveVmInstructionImage> images,
                                              const RuntimeProfileRecord& profile_record,
                                              const RuntimeProfileRecord* deferred_profile_record,
                                              DartPlantLiveVmFunctionVisitor visitor,
                                              void* user_data,
                                              DartPlantLiveVmFunctionIndexInfo* out_info) {
    if (visitor == nullptr || out_info == nullptr) {
        SetLastError("live VM function enumeration callback arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (images.empty()) {
        SetLastError("live VM function enumeration has no instruction images");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const LiveVmInstructionImage* root_image = nullptr;
    for (const auto& image : images) {
        if (image.snapshot.struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
            image.snapshot.isolate_instructions_runtime == 0 ||
            image.snapshot.isolate_instructions_size == 0) {
            SetLastError("live VM instruction image descriptor is invalid");
            return DARTPLANT_INVALID_ARGUMENT;
        }
        if (image.loading_unit_id == 1) {
            if (root_image != nullptr) {
                SetLastError("live VM instruction image set has multiple root loading units");
                return DARTPLANT_INVALID_ARGUMENT;
            }
            root_image = &image;
        }
    }
    if (root_image == nullptr) {
        SetLastError("live VM instruction image set has no root loading unit");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    ProcessMemoryReader reader = ProcessMemoryReader::VolatileSafe();
    if (!reader.Refresh()) {
        return FailProbe("cannot read /proc/self/maps for live Function index");
    }
    std::vector<CollectedLiveFunction> functions;
    DartPlantLiveVmFunctionIndexInfo info{};
    info.struct_size = sizeof(info);
    if (!CollectAllLiveFunctions(reader, profile_record, deferred_profile_record, context,
                                 root_image->snapshot, images, &functions, &info)) {
        return FailProbe("failed to enumerate live Dart Function graph");
    }
    for (const auto& function : functions) {
        if (!visitor(&function.info, user_data)) break;
    }
    *out_info = info;
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus CollectLiveVmFunctionSnapshotRecordsForImages(
    const DartPlantLiveVmContext& context, std::span<const LiveVmInstructionImage> images,
    const RuntimeProfileRecord& live_index_profile,
    const RuntimeProfileRecord& function_type_profile,
    const RuntimeProfileRecord* deferred_profile_record, DartPlantVmAdapter* observation_adapter,
    const void* observation_lease, std::vector<LiveVmFunctionSnapshotRecord>* out_records,
    DartPlantLiveVmFunctionIndexInfo* out_info) {
    if (out_records == nullptr || out_info == nullptr) {
        SetLastError("live VM semantic snapshot collector arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (images.empty()) {
        SetLastError("live VM semantic snapshot collector has no instruction images");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto reader = ProcessMemoryReader::ObservationScopedDirect(observation_adapter, context.thread,
                                                               observation_lease);
    if (!reader.has_value()) {
        SetLastError(
            "live VM semantic snapshot collector requires the current thread's exact "
            "moving-GC observation receipt");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const LiveVmInstructionImage* root_image = nullptr;
    for (const auto& image : images) {
        if (image.snapshot.struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
            image.snapshot.isolate_instructions_runtime == 0 ||
            image.snapshot.isolate_instructions_size == 0) {
            SetLastError("live VM instruction image descriptor is invalid");
            return DARTPLANT_INVALID_ARGUMENT;
        }
        if (image.loading_unit_id == 1) {
            if (root_image != nullptr) {
                SetLastError("live VM instruction image set has multiple root loading units");
                return DARTPLANT_INVALID_ARGUMENT;
            }
            root_image = &image;
        }
    }
    if (root_image == nullptr) {
        SetLastError("live VM instruction image set has no root loading unit");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    // Exact V5 observation owns the mutator in VM/outside-safepoint state, so
    // movable heap objects cannot relocate beneath this traversal. The reader
    // capability above can only select direct reads after proving the exact
    // observation receipt; unleased diagnostic/public probes remain on
    // process_vm_readv so mapping/GC races fail closed.
    if (!reader->Refresh()) {
        return FailProbe("cannot read /proc/self/maps for live Function semantic snapshot");
    }
    const auto enumeration_started = std::chrono::steady_clock::now();
    std::vector<CollectedLiveFunction> functions;
    DartPlantLiveVmFunctionIndexInfo info{};
    info.struct_size = sizeof(info);
    if (!CollectAllLiveFunctions(*reader, live_index_profile, deferred_profile_record, context,
                                 root_image->snapshot, images, &functions, &info)) {
        return FailProbe("failed to enumerate live Dart Function graph");
    }
    const auto enumeration_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - enumeration_started);
    LogLiveIndex(
        "enumeration elapsed_ms=%llu functions=%zu read_calls=%llu safe_reads=%llu bytes=%llu "
        "mode=observation_direct",
        static_cast<unsigned long long>(enumeration_elapsed.count()), functions.size(),
        static_cast<unsigned long long>(reader->read_calls()),
        static_cast<unsigned long long>(reader->safe_read_calls()),
        static_cast<unsigned long long>(reader->bytes_read()));

    const uint64_t semantic_reads_start = reader->read_calls();
    const uint64_t semantic_safe_reads_start = reader->safe_read_calls();
    const uint64_t semantic_bytes_start = reader->bytes_read();
    const auto semantics_started = std::chrono::steady_clock::now();
    size_t semantic_success = 0;
    LiveSemanticReadCache semantic_cache;
    std::vector<LiveVmFunctionSnapshotRecord> records;
    records.reserve(functions.size());
    for (const auto& collected : functions) {
        LiveVmFunctionSnapshotRecord record{};
        record.function = collected.info;
        record.signature.struct_size = sizeof(record.signature);
        if (ReadLiveVmFunctionSemanticsWithReader(
                *reader, context, function_type_profile, collected.info.function, &record.signature,
                &record.parameters, &semantic_cache) == DARTPLANT_OK) {
            record.has_semantics = true;
            ++semantic_success;
        } else {
            // Not every retained Function exposes a source-readable
            // FunctionType. Function enumeration remains valid; consumers
            // requiring semantics fail closed when this flag is false.
            ClearLastError();
        }
        records.push_back(std::move(record));
        if ((records.size() % 512) == 0 || records.size() == functions.size()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - semantics_started);
            LogLiveIndex(
                "semantics progress=%zu/%zu success=%zu elapsed_ms=%llu read_calls=%llu "
                "safe_reads=%llu bytes=%llu cache=function_type:%llu/type:%llu/string:%llu",
                records.size(), functions.size(), semantic_success,
                static_cast<unsigned long long>(elapsed.count()),
                static_cast<unsigned long long>(reader->read_calls() - semantic_reads_start),
                static_cast<unsigned long long>(reader->safe_read_calls() -
                                                semantic_safe_reads_start),
                static_cast<unsigned long long>(reader->bytes_read() - semantic_bytes_start),
                static_cast<unsigned long long>(semantic_cache.function_type_hits),
                static_cast<unsigned long long>(semantic_cache.dart_type_hits),
                static_cast<unsigned long long>(semantic_cache.dart_string_hits));
        }
    }

    *out_records = std::move(records);
    *out_info = info;
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus CollectLiveVmDeferredFunctionSnapshotRecordsForImages(
    const DartPlantLiveVmContext& context, std::span<const LiveVmInstructionImage> images,
    std::span<const uint64_t> target_runtime_image_ids,
    const RuntimeProfileRecord& live_index_profile,
    const RuntimeProfileRecord& function_type_profile, const RuntimeProfileRecord& deferred_profile,
    DartPlantVmAdapter* observation_adapter, const void* observation_lease,
    std::vector<LiveVmFunctionSnapshotRecord>* out_records,
    DartPlantLiveVmFunctionIndexInfo* out_info) {
    if (out_records == nullptr || out_info == nullptr || target_runtime_image_ids.empty()) {
        SetLastError("deferred live VM semantic delta arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto reader = ProcessMemoryReader::ObservationScopedDirect(observation_adapter, context.thread,
                                                               observation_lease);
    if (!reader.has_value()) {
        SetLastError(
            "deferred live VM semantic delta requires the current thread's exact moving-GC "
            "observation receipt");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }

    const LiveVmInstructionImage* root_image = nullptr;
    std::unordered_set<uint64_t> requested_images;
    requested_images.reserve(target_runtime_image_ids.size());
    for (const uint64_t id : target_runtime_image_ids) {
        if (id == 0 || !requested_images.insert(id).second) {
            SetLastError("deferred live VM semantic delta contains an invalid image id");
            return DARTPLANT_INVALID_ARGUMENT;
        }
    }
    std::unordered_set<uint64_t> matched_images;
    matched_images.reserve(requested_images.size());
    for (const auto& image : images) {
        if (image.snapshot.struct_size < sizeof(DartPlantFlutterSnapshotInfo) ||
            image.snapshot.isolate_instructions_runtime == 0 ||
            image.snapshot.isolate_instructions_size == 0) {
            SetLastError("deferred live VM instruction image descriptor is invalid");
            return DARTPLANT_INVALID_ARGUMENT;
        }
        if (image.loading_unit_id == 1) {
            if (root_image != nullptr) {
                SetLastError("deferred live VM image set has multiple root loading units");
                return DARTPLANT_INVALID_ARGUMENT;
            }
            root_image = &image;
        }
        if (!requested_images.contains(image.runtime_image_id)) continue;
        if (image.loading_unit_id <= 1 || !matched_images.insert(image.runtime_image_id).second) {
            SetLastError("deferred live VM semantic delta does not name exact deferred images");
            return DARTPLANT_INVALID_ARGUMENT;
        }
    }
    if (root_image == nullptr || matched_images.size() != requested_images.size()) {
        SetLastError("deferred live VM semantic delta references an unknown runtime image");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (!reader->Refresh()) {
        return FailProbe("cannot read /proc/self/maps for deferred live Function delta");
    }

    const auto& live_profile = live_index_profile.live_vm;
    const uint64_t max_cids = MaxCidCount(live_profile);
    uint64_t num_cids = 0;
    if (max_cids == 0 ||
        !reader->Read(
            static_cast<uintptr_t>(context.class_table) + live_profile.class_table_num_cids_offset,
            &num_cids) ||
        num_cids == 0 || num_cids > max_cids || num_cids > UINT32_MAX) {
        return FailProbe("cannot build stable ClassTable identity for deferred Function delta");
    }
    std::unordered_map<uint64_t, uint32_t> class_ids_by_object;
    class_ids_by_object.reserve(static_cast<size_t>(num_cids));
    for (uint64_t cid = 1; cid < num_cids; ++cid) {
        uint64_t tagged_class = 0;
        if (!reader->Read(
                static_cast<uintptr_t>(context.cached_class_table_table) + cid * sizeof(uint64_t),
                &tagged_class)) {
            return FailProbe("cannot read ClassTable owner identity for deferred Function delta");
        }
        if (tagged_class == 0) continue;
        if (!class_ids_by_object.emplace(tagged_class, static_cast<uint32_t>(cid)).second) {
            return FailProbe("ClassTable owner identity is ambiguous for deferred Function delta");
        }
    }
    uint64_t libraries = 0;
    if (!reader->Read(static_cast<uintptr_t>(context.object_store) +
                          live_profile.object_store_libraries_offset,
                      &libraries) ||
        !RequireCid(*reader, live_profile, libraries, live_profile.cid_growable_object_array)) {
        return FailProbe("cannot read library owner identities for deferred Function delta");
    }
    const uintptr_t growable = Untag(live_profile, libraries);
    uint64_t library_count = 0;
    uint64_t library_data = 0;
    if (!ReadPositiveCompressedSmi(*reader,
                                   growable + live_profile.growable_object_array_length_offset,
                                   live_profile, &library_count) ||
        library_count > kMaxClassFunctions ||
        !ReadCompressedObject(*reader, growable, live_profile,
                              live_profile.growable_object_array_data_offset, context.heap_base,
                              &library_data)) {
        return FailProbe("cannot decode library owner identities for deferred Function delta");
    }
    class_ids_by_object.reserve(class_ids_by_object.size() + static_cast<size_t>(library_count));
    const RawObjectLayout* raw = FindRawObjectLayout(live_profile.profile_version);
    if (raw == nullptr) {
        return FailProbe("cannot decode library array layout for deferred Function delta");
    }
    uint64_t library_data_length = 0;
    if (!VisitArrayRawElements(
            *reader, live_profile, library_data, kMaxClassFunctions,
            [&](uint64_t index, uint32_t compressed) {
                if (index >= library_count) return ArrayVisitDecision::kStop;
                if ((compressed & raw->smi_tag_mask) != raw->heap_object_tag) {
                    return ArrayVisitDecision::kContinue;
                }
                const uint64_t library = DecompressObject(context.heap_base, compressed);
                if (!RequireCid(*reader, live_profile, library, live_profile.cid_library)) {
                    return ArrayVisitDecision::kContinue;
                }
                uint64_t top_level_class = 0;
                if (!ReadCompressedObject(*reader, Untag(live_profile, library), live_profile,
                                          live_profile.library_toplevel_class_offset,
                                          context.heap_base, &top_level_class)) {
                    return ArrayVisitDecision::kContinue;
                }
                const auto stable_owner_id = StableTopLevelOwnerId(index);
                if (!stable_owner_id.has_value()) {
                    return ArrayVisitDecision::kFail;
                }
                const auto [found, inserted] =
                    class_ids_by_object.emplace(top_level_class, *stable_owner_id);
                if (!inserted && found->second != *stable_owner_id) {
                    return ArrayVisitDecision::kFail;
                }
                return ArrayVisitDecision::kContinue;
            },
            &library_data_length) ||
        library_data_length < library_count) {
        return FailProbe(
            "library owner identity array is inconsistent for deferred Function delta");
    }

    std::unordered_set<uint64_t> seen_functions;
    std::vector<CollectedLiveFunction> functions;
    uint32_t skipped = 0;
    const auto started = std::chrono::steady_clock::now();
    if (!CollectDeferredLoadingUnitFunctions(
            *reader, deferred_profile, live_index_profile, context, root_image->snapshot, images,
            target_runtime_image_ids, class_ids_by_object, &seen_functions, &functions, &skipped)) {
        return FailProbe("failed to enumerate newly-loaded deferred Functions");
    }
    // Delta publication has no safe way to infer a missing changed Function.
    // Any rejected Function therefore makes this optimization ineligible; the
    // caller will rebuild the complete graph under the same observation.
    if (skipped != 0) {
        SetLastError("deferred Function delta was incomplete; full live graph rebuild required");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    DartPlantLiveVmFunctionIndexInfo info{};
    info.struct_size = sizeof(info);
    FinalizeCollectedLiveFunctionAliases(&functions, 0, &info);

    std::vector<LiveVmFunctionSnapshotRecord> records;
    records.reserve(functions.size());
    size_t semantic_success = 0;
    LiveSemanticReadCache semantic_cache;
    for (const auto& collected : functions) {
        if (collected.info.owner_class_id == 0 ||
            collected.info.owner_function_index == UINT32_MAX) {
            SetLastError("deferred Function delta has no stable owner-slot receipt");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        LiveVmFunctionSnapshotRecord record{};
        record.function = collected.info;
        record.signature.struct_size = sizeof(record.signature);
        if (ReadLiveVmFunctionSemanticsWithReader(
                *reader, context, function_type_profile, collected.info.function, &record.signature,
                &record.parameters, &semantic_cache) == DARTPLANT_OK) {
            record.has_semantics = true;
            ++semantic_success;
        } else {
            ClearLastError();
        }
        records.push_back(std::move(record));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    LogLiveIndex(
        "deferred delta images=%zu functions=%zu semantics=%zu elapsed_ms=%llu read_calls=%llu "
        "safe_reads=%llu bytes=%llu cache=function_type:%llu/type:%llu/string:%llu",
        target_runtime_image_ids.size(), records.size(), semantic_success,
        static_cast<unsigned long long>(elapsed.count()),
        static_cast<unsigned long long>(reader->read_calls()),
        static_cast<unsigned long long>(reader->safe_read_calls()),
        static_cast<unsigned long long>(reader->bytes_read()),
        static_cast<unsigned long long>(semantic_cache.function_type_hits),
        static_cast<unsigned long long>(semantic_cache.dart_type_hits),
        static_cast<unsigned long long>(semantic_cache.dart_string_hits));

    *out_records = std::move(records);
    *out_info = info;
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

    dartplant::LiveVmCandidateResolution resolution{};
    const DartPlantStatus status = dartplant::ResolveLiveVmCandidateForRegisters(
        *snapshot, *registers, &resolution, out_context);
    if (status != DARTPLANT_OK) return status;
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

    dartplant::LiveVmCandidateResolution resolution{};
    DartPlantStatus status = dartplant::ResolveLiveVmCandidateForArm64Context(
        *snapshot, *invocation->context, &resolution);
    if (status != DARTPLANT_OK) return status;
    const dartplant::RuntimeProfileRecord* record = resolution.profile;
    DartPlantLiveVmProfile profile = record->live_vm;
    const dartplant::RawObjectLayout* raw = &record->raw_object;

    dartplant::ProcessMemoryReader reader = dartplant::ProcessMemoryReader::VolatileSafe();
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

    if (info.thread == 0) {
        return dartplant::FailProbe("THR does not point to a readable Dart Thread layout");
    }

    const dartplant::vm_abi::RootProof& roots = resolution.probe.roots;
    const uint64_t thread_null = roots.thread_null;
    info.heap_base = roots.heap_base;
    info.isolate = roots.isolate;
    info.isolate_group = roots.isolate_group;
    info.global_object_pool = roots.global_object_pool;
    info.class_table = roots.class_table;
    info.cached_class_table_table = roots.cached_class_table_table;
    info.object_store = roots.object_store;
    info.object_pool_length = roots.object_pool_length;
    info.heap_bits_match = roots.heap_bits_match ? 1 : 0;
    info.null_register_match = roots.null_register_match ? 1 : 0;
    info.thread_pool_match = roots.thread_pool_match ? 1 : 0;

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
    const auto function_code_entry_proof = dartplant::vm_abi::ProveFunctionCodeEntry(
        *record, info.heap_base, info.function, info.code, target_entry,
        dartplant::HasSnapshotFeature(snapshot->snapshot_features, "dedup_instructions"));
    if (!function_code_entry_proof.passed) {
        return dartplant::FailProbe("Function -> Code -> entry relational proof failed");
    }
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
    const dartplant::RuntimeProfileRecord* record =
        dartplant::FindRuntimeProfileByVersion(profile.profile_version);
    if (record == nullptr) {
        return dartplant::FailProbe("live VM method has no source-verified ABI record");
    }

    dartplant::ProcessMemoryReader reader = dartplant::ProcessMemoryReader::VolatileSafe();
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
    const auto function_code_entry_proof = dartplant::vm_abi::ProveFunctionCodeEntry(
        *record, context->heap_base, method.function, method.code, method.function_entry_point,
        dartplant::HasSnapshotFeature(snapshot->snapshot_features, "dedup_instructions"));
    if (!function_code_entry_proof.passed) {
        return dartplant::FailProbe("Function -> Code -> entry relational proof failed");
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
    dartplant::ProcessMemoryReader reader = dartplant::ProcessMemoryReader::VolatileSafe();
    DartPlantStatus status = dartplant::PrepareFunctionSignatureRead(*context, *snapshot, function,
                                                                     &profile, &layout, &reader);
    if (status != DARTPLANT_OK) return status;

    dartplant::ParsedFunctionSignature parsed{};
    status = dartplant::ParseRetainedFunctionSignatureWithRetry(
        reader, profile, *layout, context->heap_base, function, &parsed);
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
    dartplant::ProcessMemoryReader reader = dartplant::ProcessMemoryReader::VolatileSafe();
    DartPlantStatus status = dartplant::PrepareFunctionSignatureRead(*context, *snapshot, function,
                                                                     &profile, &layout, &reader);
    if (status != DARTPLANT_OK) return status;

    dartplant::ParsedFunctionSignature parsed{};
    status = dartplant::ParseRetainedFunctionSignatureWithRetry(
        reader, profile, *layout, context->heap_base, function, &parsed);
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
    const dartplant::RuntimeProfileRecord* record =
        dartplant::FindRuntimeProfileByVersion(profile.profile_version);
    if (record == nullptr) {
        return dartplant::FailProbe("live VM function index has no source-verified ABI record");
    }
    return dartplant::VisitLiveVmFunctionsForProfile(*context, *snapshot, *record, visitor,
                                                     user_data, out_info);
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

    dartplant::ProcessMemoryReader reader = dartplant::ProcessMemoryReader::VolatileSafe();
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
