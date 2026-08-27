// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "dartplant/live_vm.h"

#include <algorithm>
#include <array>
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

namespace dartplant {
namespace {

constexpr uint64_t kHeapObjectTag = 1;
constexpr uint64_t kSmiTagMask = 1;
constexpr uint32_t kClassIdTagShift = 12;
constexpr uint64_t kClassIdTagMask = (1ULL << 20) - 1;
constexpr uint64_t kMaxObjectPoolEntries = 1ULL << 24;
constexpr uint64_t kMaxClassFunctions = 1ULL << 20;

// Dart 3.4.4 / Flutter 3.22.x ARM64 PRODUCT + compressed pointers.
//
// Register assignments come from the vendored Dart SDK constants_arm64.h.
// Thread/Code/Function/Array/String/IsolateGroup offsets come from the
// PRODUCT + TARGET_ARCH_ARM64 + DART_COMPRESSED_POINTERS block in
// runtime_offsets_extracted.h. Private heap-object fields not emitted by the
// compiler offset extractor are data-only offsets derived from raw_object.h for
// the same build profile. DartPlant never includes or links these private VM
// headers at runtime.
constexpr DartPlantLiveVmProfile kDart344Arm64ProductCompressed = {
    .struct_size = sizeof(DartPlantLiveVmProfile),
    .profile_version = 1,
    .name = "dart-3.4.4-arm64-product-compressed",
    .dart_version = "3.4.4",
    .snapshot_hash = "d20a1be77c3d3c41b2a5accaee1ce549",
    .snapshot_profile = "flutter-arm64-product-compressed",
    .thr_register = 26,
    .pp_register = 27,
    .code_register = 24,
    .heap_bits_register = 28,
    .null_register = 22,
    .reserved_registers = {0, 0, 0},
    .thread_heap_base_offset = 0x48,
    .thread_object_null_offset = 0x70,
    .thread_global_object_pool_offset = 0x758,
    .thread_isolate_offset = 0x6f0,
    .thread_isolate_group_offset = 0x6f8,
    .isolate_group_class_table_offset = 0x10,
    .isolate_group_cached_class_table_table_offset = 0x18,
    .isolate_group_object_store_offset = 0x20,
    .class_table_num_cids_offset = 0x10,
    .object_store_libraries_offset = 0x3a8,
    .code_entry_point_offset = 0x8,
    .code_object_pool_offset = 0x28,
    .code_owner_offset = 0x38,
    .code_instructions_length_offset = 0x74,
    .function_entry_point_offset = 0x8,
    .function_name_offset = 0x18,
    .function_owner_offset = 0x1c,
    .function_code_offset = 0x2c,
    .function_kind_tag_offset = 0x30,
    .class_name_offset = 0x8,
    .class_functions_offset = 0xc,
    .class_library_offset = 0x24,
    .library_url_offset = 0xc,
    .library_toplevel_class_offset = 0x1c,
    .array_length_offset = 0xc,
    .array_elements_offset = 0x10,
    .growable_object_array_length_offset = 0xc,
    .growable_object_array_data_offset = 0x10,
    .string_length_offset = 0x8,
    .string_data_offset = 0x10,
    .object_pool_length_offset = 0x8,
    .object_pool_elements_offset = 0x10,
    .cid_class = 5,
    .cid_function = 7,
    .cid_library = 13,
    .cid_code = 18,
    .cid_object_pool = 22,
    .cid_array = 89,
    .cid_immutable_array = 90,
    .cid_growable_object_array = 91,
    .cid_one_byte_string = 93,
    .cid_two_byte_string = 94,
};

// Dart 3.5.0 PRODUCT ARM64 compressed profile. Thread offsets come from the
// exact 3.5.0 runtime_offsets_extracted.h block; ObjectStore.libraries is
// derived from that tag's OBJECT_STORE_FIELD_LIST using ObjectStore.int_type as
// the exported anchor (0x130 -> libraries 0x380). Heap-object layouts used here
// are unchanged from the 3.4 AOT compressed layout.
constexpr DartPlantLiveVmProfile kDart350Arm64ProductCompressed = {
    .struct_size = sizeof(DartPlantLiveVmProfile),
    .profile_version = 2,
    .name = "dart-3.5.0-arm64-product-compressed",
    .dart_version = "3.5.0",
    .snapshot_hash = "80a49c7111088100a233b2ae788e1f48",
    .snapshot_profile = "flutter-arm64-product-compressed",
    .thr_register = 26,
    .pp_register = 27,
    .code_register = 24,
    .heap_bits_register = 28,
    .null_register = 22,
    .reserved_registers = {0, 0, 0},
    .thread_heap_base_offset = 0x48,
    .thread_object_null_offset = 0x78,
    .thread_global_object_pool_offset = 0x778,
    .thread_isolate_offset = 0x708,
    .thread_isolate_group_offset = 0x710,
    .isolate_group_class_table_offset = 0x10,
    .isolate_group_cached_class_table_table_offset = 0x18,
    .isolate_group_object_store_offset = 0x20,
    .class_table_num_cids_offset = 0x10,
    .object_store_libraries_offset = 0x380,
    .code_entry_point_offset = 0x8,
    .code_object_pool_offset = 0x28,
    .code_owner_offset = 0x38,
    .code_instructions_length_offset = 0x74,
    .function_entry_point_offset = 0x8,
    .function_name_offset = 0x18,
    .function_owner_offset = 0x1c,
    .function_code_offset = 0x2c,
    .function_kind_tag_offset = 0x30,
    .class_name_offset = 0x8,
    .class_functions_offset = 0xc,
    .class_library_offset = 0x24,
    .library_url_offset = 0xc,
    .library_toplevel_class_offset = 0x1c,
    .array_length_offset = 0xc,
    .array_elements_offset = 0x10,
    .growable_object_array_length_offset = 0xc,
    .growable_object_array_data_offset = 0x10,
    .string_length_offset = 0x8,
    .string_data_offset = 0x10,
    .object_pool_length_offset = 0x8,
    .object_pool_elements_offset = 0x10,
    .cid_class = 5,
    .cid_function = 7,
    .cid_library = 13,
    .cid_code = 18,
    .cid_object_pool = 22,
    .cid_array = 89,
    .cid_immutable_array = 90,
    .cid_growable_object_array = 91,
    .cid_one_byte_string = 93,
    .cid_two_byte_string = 94,
};

// Dart 3.12.1 PRODUCT ARM64 compressed profile. Dart 3.12 inserts Bytecode
// after Code in the predefined CID list, shifting ObjectPool and later CIDs by
// one. ObjectStore.libraries is derived from the 3.12.1 field list using the
// exported ObjectStore.int_type=0x168 anchor (result 0x3e0).
constexpr DartPlantLiveVmProfile kDart3121Arm64ProductCompressed = {
    .struct_size = sizeof(DartPlantLiveVmProfile),
    .profile_version = 3,
    .name = "dart-3.12.1-arm64-product-compressed",
    .dart_version = "3.12.1",
    .snapshot_hash = "ace654289f5abc240509fc941453ebc5",
    .snapshot_profile = "flutter-arm64-product-compressed",
    .thr_register = 26,
    .pp_register = 27,
    .code_register = 24,
    .heap_bits_register = 28,
    .null_register = 22,
    .reserved_registers = {0, 0, 0},
    .thread_heap_base_offset = 0x58,
    .thread_object_null_offset = 0x88,
    .thread_global_object_pool_offset = 0x6e0,
    .thread_isolate_offset = 0x680,
    .thread_isolate_group_offset = 0x688,
    .isolate_group_class_table_offset = 0x10,
    .isolate_group_cached_class_table_table_offset = 0x18,
    .isolate_group_object_store_offset = 0x20,
    .class_table_num_cids_offset = 0x10,
    .object_store_libraries_offset = 0x3e0,
    .code_entry_point_offset = 0x8,
    .code_object_pool_offset = 0x28,
    .code_owner_offset = 0x38,
    .code_instructions_length_offset = 0x74,
    .function_entry_point_offset = 0x8,
    .function_name_offset = 0x18,
    .function_owner_offset = 0x1c,
    .function_code_offset = 0x2c,
    .function_kind_tag_offset = 0x30,
    .class_name_offset = 0x8,
    .class_functions_offset = 0xc,
    .class_library_offset = 0x24,
    .library_url_offset = 0xc,
    .library_toplevel_class_offset = 0x1c,
    .array_length_offset = 0xc,
    .array_elements_offset = 0x10,
    .growable_object_array_length_offset = 0xc,
    .growable_object_array_data_offset = 0x10,
    .string_length_offset = 0x8,
    .string_data_offset = 0x10,
    .object_pool_length_offset = 0x8,
    .object_pool_elements_offset = 0x10,
    .cid_class = 5,
    .cid_function = 7,
    .cid_library = 13,
    .cid_code = 18,
    .cid_object_pool = 23,
    .cid_array = 90,
    .cid_immutable_array = 91,
    .cid_growable_object_array = 92,
    .cid_one_byte_string = 94,
    .cid_two_byte_string = 95,
};

constexpr std::array<const DartPlantLiveVmProfile*, 3> kLiveVmProfiles = {
    &kDart344Arm64ProductCompressed,
    &kDart350Arm64ProductCompressed,
    &kDart3121Arm64ProductCompressed,
};

struct CanonicalBoolLayout {
    uint32_t profile_version;
    uint32_t thread_true_offset;
    uint32_t thread_false_offset;
    uint32_t value_offset;
    uint32_t cid;
};

// Dart SDK runtime_offsets_extracted.h, PRODUCT + TARGET_ARCH_ARM64 +
// DART_COMPRESSED_POINTERS, plus raw_object.h's UntaggedBool::value_ layout.
constexpr std::array<CanonicalBoolLayout, 3> kCanonicalBoolLayouts = {{
    {1, 0x78, 0x80, 0x8, 62},  // Dart 3.4.4.
    {2, 0x80, 0x88, 0x8, 62},  // Dart 3.5.0.
    {3, 0x98, 0xa0, 0x8, 63},  // Dart 3.12.1.
}};

const CanonicalBoolLayout* FindCanonicalBoolLayout(uint32_t profile_version) {
    const auto found = std::find_if(kCanonicalBoolLayouts.begin(), kCanonicalBoolLayouts.end(),
                                    [profile_version](const CanonicalBoolLayout& layout) {
                                        return layout.profile_version == profile_version;
                                    });
    return found == kCanonicalBoolLayouts.end() ? nullptr : &*found;
}

struct FunctionTypeLayout {
    uint32_t profile_version;
    uint32_t function_signature_offset;
    uint32_t abstract_type_flags_offset;
    uint32_t type_parameters_offset;
    uint32_t result_type_offset;
    uint32_t parameter_types_offset;
    uint32_t named_parameter_names_offset;
    uint32_t packed_parameter_counts_offset;
    uint32_t packed_type_parameter_counts_offset;
    uint32_t cid_type;
    uint32_t cid_function_type;
    uint32_t cid_record_type;
    uint32_t cid_type_parameter;
    uint32_t cid_null;
    uint32_t cid_dynamic;
    uint32_t cid_void;
    uint32_t cid_never;
    uint32_t type_parameter_base_offset;
    uint32_t type_parameter_index_offset;
    uint8_t nullability_bits;
    uint8_t type_class_id_shift;
    uint8_t type_parameter_function_bit;
};

// Dart SDK raw_object.h + runtime_offsets_extracted.h for PRODUCT ARM64 with
// compressed pointers. FunctionType.result_type is the compressed field between
// type_parameters (0x20) and parameter_types (0x28); it is not emitted by the
// compiler offset extractor because generated code does not address it directly.
constexpr std::array<FunctionTypeLayout, 3> kFunctionTypeLayouts = {{
    {1,  0x20, 0x10, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 48, 49,
     50, 51,   170,  171,  172,  173,  0x24, 0x26, 2,    4,  4},  // Dart 3.4.4.
    {2,  0x20, 0x10, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 48, 49,
     50, 51,   170,  171,  172,  173,  0x24, 0x26, 1,    3,  3},  // Dart 3.5.0.
    {3,  0x20, 0x10, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 49, 50, 51,
     52, 171,  172,  173,  174,  0x24, 0x26, 1,    3,    3},  // Dart 3.12.1; Bytecode shifts later
                                                              // predefined CIDs.
}};

const FunctionTypeLayout* FindFunctionTypeLayout(uint32_t profile_version) {
    const auto found = std::find_if(kFunctionTypeLayouts.begin(), kFunctionTypeLayouts.end(),
                                    [profile_version](const FunctionTypeLayout& layout) {
                                        return layout.profile_version == profile_version;
                                    });
    return found == kFunctionTypeLayouts.end() ? nullptr : &*found;
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
        if (out_value == nullptr || !Contains(address, sizeof(T))) return false;
        std::memcpy(out_value, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }

    bool ReadBytes(uintptr_t address, void* output, size_t size) const {
        if (output == nullptr || !Contains(address, size)) return false;
        std::memcpy(output, reinterpret_cast<const void*>(address), size);
        return true;
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

bool IsHeapObject(uint64_t tagged) { return (tagged & kSmiTagMask) == kHeapObjectTag; }

uintptr_t Untag(uint64_t tagged) { return static_cast<uintptr_t>(tagged - kHeapObjectTag); }

uint64_t ObjectPoolOffsetFromIndex(const DartPlantLiveVmProfile& profile, uint32_t index) {
    // Dart ObjectPool::OffsetFromIndex(): element_offset(index) - kHeapObjectTag.
    // ObjectPool entries are native-word-sized even when Dart heap pointers are
    // compressed; the tagged-object payload stored in an entry remains a full
    // ObjectPtr-sized word.
    return static_cast<uint64_t>(profile.object_pool_elements_offset) - kHeapObjectTag +
           static_cast<uint64_t>(index) * sizeof(uint64_t);
}

bool ObjectPoolIndexFromOffset(const DartPlantLiveVmProfile& profile, uint64_t offset,
                               uint32_t* out_index) {
    if (out_index == nullptr) return false;
    const uint64_t first =
        static_cast<uint64_t>(profile.object_pool_elements_offset) - kHeapObjectTag;
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

bool ReadCid(const ProcessMemoryReader& reader, uint64_t tagged, uint32_t* out_cid) {
    if (out_cid == nullptr || !IsHeapObject(tagged)) return false;
    uint64_t tags = 0;
    if (!reader.Read(Untag(tagged), &tags)) return false;
    *out_cid = static_cast<uint32_t>((tags >> kClassIdTagShift) & kClassIdTagMask);
    return true;
}

bool RequireCid(const ProcessMemoryReader& reader, uint64_t tagged, uint32_t expected) {
    uint32_t cid = 0;
    return ReadCid(reader, tagged, &cid) && cid == expected;
}

bool ReadCompressedObject(const ProcessMemoryReader& reader, uintptr_t object_address,
                          uint32_t offset, uint64_t heap_base, uint64_t* out_tagged) {
    if (out_tagged == nullptr) return false;
    uint32_t compressed = 0;
    if (!reader.Read(object_address + offset, &compressed) ||
        (compressed & kSmiTagMask) != kHeapObjectTag) {
        return false;
    }
    *out_tagged = DecompressObject(heap_base, compressed);
    return true;
}

bool ReadPositiveCompressedSmi(const ProcessMemoryReader& reader, uintptr_t address,
                               uint64_t* out_value) {
    if (out_value == nullptr) return false;
    uint32_t raw = 0;
    if (!reader.Read(address, &raw) || (raw & kSmiTagMask) != 0) return false;
    *out_value = static_cast<uint64_t>(raw >> 1);
    return true;
}

bool ReadDartString(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                    uint64_t tagged, char* output, size_t capacity) {
    if (output == nullptr || capacity == 0 || !IsHeapObject(tagged)) return false;
    output[0] = '\0';
    uint32_t cid = 0;
    if (!ReadCid(reader, tagged, &cid) ||
        (cid != profile.cid_one_byte_string && cid != profile.cid_two_byte_string)) {
        return false;
    }
    const uintptr_t object = Untag(tagged);
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, object + profile.string_length_offset, &length) ||
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
    if (!ReadCid(reader, tagged_array, &cid) ||
        (cid != profile.cid_array && cid != profile.cid_immutable_array)) {
        return false;
    }
    const uintptr_t array = Untag(tagged_array);
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, array + profile.array_length_offset, &length) ||
        index >= length) {
        return false;
    }
    uint32_t compressed = 0;
    if (!reader.Read(array + profile.array_elements_offset + index * sizeof(uint32_t),
                     &compressed) ||
        (compressed & kSmiTagMask) != kHeapObjectTag) {
        return false;
    }
    *out_tagged = DecompressObject(heap_base, compressed);
    return true;
}

bool ReadArrayLength(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                     uint64_t tagged_array, uint64_t* out_length) {
    if (out_length == nullptr) return false;
    uint32_t cid = 0;
    return ReadCid(reader, tagged_array, &cid) &&
           (cid == profile.cid_array || cid == profile.cid_immutable_array) &&
           ReadPositiveCompressedSmi(reader, Untag(tagged_array) + profile.array_length_offset,
                                     out_length);
}

bool ReadArrayRawElement(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                         uint64_t tagged_array, uint64_t index, uint32_t* out_raw) {
    if (out_raw == nullptr) return false;
    uint64_t length = 0;
    if (!ReadArrayLength(reader, profile, tagged_array, &length) || index >= length) return false;
    return reader.Read(
        Untag(tagged_array) + profile.array_elements_offset + index * sizeof(uint32_t), out_raw);
}

bool ArrayContainsFunction(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                           uint64_t heap_base, uint64_t tagged_array, uint64_t tagged_function) {
    uint32_t cid = 0;
    if (!ReadCid(reader, tagged_array, &cid) ||
        (cid != profile.cid_array && cid != profile.cid_immutable_array)) {
        return false;
    }
    const uintptr_t array = Untag(tagged_array);
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, array + profile.array_length_offset, &length) ||
        length > kMaxClassFunctions) {
        return false;
    }
    for (uint64_t index = 0; index < length; ++index) {
        uint32_t compressed = 0;
        const uintptr_t slot = array + profile.array_elements_offset + index * sizeof(uint32_t);
        if (!reader.Read(slot, &compressed)) return false;
        if ((compressed & kSmiTagMask) != kHeapObjectTag) continue;
        if (DecompressObject(heap_base, compressed) == tagged_function) return true;
    }
    return false;
}

bool IsClosureFunction(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                       uint64_t tagged_function) {
    uint32_t kind_tag = 0;
    if (!reader.Read(Untag(tagged_function) + profile.function_kind_tag_offset, &kind_tag)) {
        return true;
    }
    const uint32_t kind = kind_tag & 0x1f;
    return kind == 1 || kind == 2;  // ClosureFunction / ImplicitClosureFunction.
}

bool FunctionNameMatches(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                         uint64_t heap_base, uint64_t tagged_function, const char* expected_name) {
    if (expected_name == nullptr || expected_name[0] == '\0') return true;
    uint64_t tagged_name = 0;
    char name[DARTPLANT_LIVE_VM_FUNCTION_NAME_MAX] = {};
    return ReadCompressedObject(reader, Untag(tagged_function), profile.function_name_offset,
                                heap_base, &tagged_name) &&
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
    return ReadCompressedObject(reader, Untag(tagged_class), profile.class_name_offset, heap_base,
                                &tagged_name) &&
           ReadDartString(reader, profile, tagged_name, name, sizeof(name)) &&
           std::strcmp(name, expected_class) == 0;
}

bool ReadClassLibrary(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                      uint64_t heap_base, uint64_t tagged_class, uint64_t* out_library) {
    return out_library != nullptr && RequireCid(reader, tagged_class, profile.cid_class) &&
           ReadCompressedObject(reader, Untag(tagged_class), profile.class_library_offset,
                                heap_base, out_library) &&
           RequireCid(reader, *out_library, profile.cid_library);
}

bool ReadLibraryUri(const ProcessMemoryReader& reader, const DartPlantLiveVmProfile& profile,
                    uint64_t heap_base, uint64_t tagged_library, char* output, size_t capacity) {
    if (!RequireCid(reader, tagged_library, profile.cid_library)) return false;
    uint64_t tagged_url = 0;
    return ReadCompressedObject(reader, Untag(tagged_library), profile.library_url_offset,
                                heap_base, &tagged_url) &&
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
    if (!ReadCompressedObject(reader, Untag(tagged_class), profile.class_functions_offset,
                              heap_base, &functions)) {
        return false;
    }
    uint32_t array_cid = 0;
    if (!ReadCid(reader, functions, &array_cid) ||
        (array_cid != profile.cid_array && array_cid != profile.cid_immutable_array)) {
        return false;
    }
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, Untag(functions) + profile.array_length_offset,
                                   &length) ||
        length > kMaxClassFunctions) {
        return false;
    }
    for (uint64_t index = 0; index < length; ++index) {
        uint64_t function = 0;
        if (!ReadArrayElement(reader, profile, heap_base, functions, index, &function) ||
            !RequireCid(reader, function, profile.cid_function)) {
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
    if (class_table != 0 && cached_class_table_table != 0 &&
        reader.Read(class_table + profile.class_table_num_cids_offset, &num_cids) && num_cids > 0 &&
        num_cids <= kClassIdTagMask + 1) {
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
        !RequireCid(reader, libraries, profile.cid_growable_object_array)) {
        return false;
    }
    const uintptr_t growable = Untag(libraries);
    uint64_t length = 0;
    uint64_t data = 0;
    if (!ReadPositiveCompressedSmi(reader, growable + profile.growable_object_array_length_offset,
                                   &length) ||
        length > kMaxClassFunctions ||
        !ReadCompressedObject(reader, growable, profile.growable_object_array_data_offset,
                              heap_base, &data)) {
        return false;
    }
    for (uint64_t index = 0; index < length; ++index) {
        uint64_t library = 0;
        if (!ReadArrayElement(reader, profile, heap_base, data, index, &library) ||
            !RequireCid(reader, library, profile.cid_library) ||
            !LibraryIdentityMatches(reader, profile, heap_base, library, expected_library)) {
            continue;
        }
        uint64_t top_level_class = 0;
        if (!ReadCompressedObject(reader, Untag(library), profile.library_toplevel_class_offset,
                                  heap_base, &top_level_class)) {
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

bool ScanFunctionsByEntryInClass(const ProcessMemoryReader& reader,
                                 const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                                 uint64_t tagged_class, bool is_top_level, uintptr_t target_entry,
                                 const char* expected_function, const char* expected_class,
                                 uint64_t* selected_function, uint32_t* alias_count) {
    if (selected_function == nullptr || alias_count == nullptr ||
        !RequireCid(reader, tagged_class, profile.cid_class)) {
        return false;
    }
    uint64_t functions = 0;
    if (!ReadCompressedObject(reader, Untag(tagged_class), profile.class_functions_offset,
                              heap_base, &functions)) {
        return false;
    }
    uint32_t array_cid = 0;
    if (!ReadCid(reader, functions, &array_cid) ||
        (array_cid != profile.cid_array && array_cid != profile.cid_immutable_array)) {
        return false;
    }
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, Untag(functions) + profile.array_length_offset,
                                   &length) ||
        length > kMaxClassFunctions) {
        return false;
    }
    for (uint64_t index = 0; index < length; ++index) {
        uint64_t function = 0;
        if (!ReadArrayElement(reader, profile, heap_base, functions, index, &function) ||
            !RequireCid(reader, function, profile.cid_function)) {
            continue;
        }
        uint64_t entry = 0;
        if (!reader.Read(Untag(function) + profile.function_entry_point_offset, &entry) ||
            entry != target_entry) {
            continue;
        }
        if (*alias_count != std::numeric_limits<uint32_t>::max()) ++*alias_count;
        if (*selected_function == 0 && !IsClosureFunction(reader, profile, function) &&
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
                                 const char* expected_function, const char* expected_class,
                                 uint64_t* out_function, uint32_t* out_alias_count) {
    if (out_function == nullptr || out_alias_count == nullptr || target_entry == 0) return false;
    *out_function = 0;
    *out_alias_count = 0;

    // Normal classes: cached_class_table_table is an atomic ClassPtr* published
    // specifically for fast readers. Its entries are full Dart tagged pointers.
    uint64_t num_cids = 0;
    if (class_table != 0 && cached_class_table_table != 0 &&
        reader.Read(class_table + profile.class_table_num_cids_offset, &num_cids) && num_cids > 0 &&
        num_cids <= kClassIdTagMask + 1) {
        for (uint64_t cid = 1; cid < num_cids; ++cid) {
            uint64_t tagged_class = 0;
            if (!reader.Read(cached_class_table_table + cid * sizeof(uint64_t), &tagged_class) ||
                tagged_class == 0) {
                continue;
            }
            ScanFunctionsByEntryInClass(reader, profile, heap_base, tagged_class, false,
                                        target_entry, expected_function, expected_class,
                                        out_function, out_alias_count);
        }
    }

    // Top-level classes do not live in the normal cached class table. Walk the
    // ObjectStore's GrowableObjectArray of libraries, then each library's
    // toplevel_class. ObjectStore fields are full tagged Dart pointers.
    uint64_t libraries = 0;
    if (object_store == 0 ||
        !reader.Read(object_store + profile.object_store_libraries_offset, &libraries) ||
        !RequireCid(reader, libraries, profile.cid_growable_object_array)) {
        return *out_function != 0;
    }
    const uintptr_t growable = Untag(libraries);
    uint64_t length = 0;
    uint64_t data = 0;
    if (!ReadPositiveCompressedSmi(reader, growable + profile.growable_object_array_length_offset,
                                   &length) ||
        length > kMaxClassFunctions ||
        !ReadCompressedObject(reader, growable, profile.growable_object_array_data_offset,
                              heap_base, &data)) {
        return *out_function != 0;
    }
    for (uint64_t index = 0; index < length; ++index) {
        uint64_t library = 0;
        if (!ReadArrayElement(reader, profile, heap_base, data, index, &library) ||
            !RequireCid(reader, library, profile.cid_library)) {
            continue;
        }
        uint64_t top_level_class = 0;
        if (!ReadCompressedObject(reader, Untag(library), profile.library_toplevel_class_offset,
                                  heap_base, &top_level_class)) {
            continue;
        }
        ScanFunctionsByEntryInClass(reader, profile, heap_base, top_level_class, true, target_entry,
                                    expected_function, expected_class, out_function,
                                    out_alias_count);
    }
    return *out_function != 0;
}

struct CollectedLiveFunction {
    DartPlantLiveVmFunctionInfo info{};
};

bool CollectFunctionsInClass(const ProcessMemoryReader& reader,
                             const DartPlantLiveVmProfile& profile, uint64_t heap_base,
                             uint64_t tagged_class, bool is_top_level,
                             const DartPlantFlutterSnapshotInfo& snapshot,
                             std::unordered_set<uint64_t>* seen_functions,
                             std::vector<CollectedLiveFunction>* functions,
                             uint32_t* skipped_function_count) {
    if (seen_functions == nullptr || functions == nullptr || skipped_function_count == nullptr ||
        !RequireCid(reader, tagged_class, profile.cid_class)) {
        return false;
    }

    uint64_t library = 0;
    uint64_t class_functions = 0;
    if (!ReadClassLibrary(reader, profile, heap_base, tagged_class, &library) ||
        !ReadCompressedObject(reader, Untag(tagged_class), profile.class_functions_offset,
                              heap_base, &class_functions)) {
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
        if (!ReadCompressedObject(reader, Untag(tagged_class), profile.class_name_offset, heap_base,
                                  &class_name_object) ||
            !ReadDartString(reader, profile, class_name_object, class_name, sizeof(class_name))) {
            return false;
        }
    }

    uint32_t functions_cid = 0;
    if (!ReadCid(reader, class_functions, &functions_cid) ||
        (functions_cid != profile.cid_array && functions_cid != profile.cid_immutable_array)) {
        return false;
    }
    uint64_t length = 0;
    if (!ReadPositiveCompressedSmi(reader, Untag(class_functions) + profile.array_length_offset,
                                   &length) ||
        length > kMaxClassFunctions) {
        return false;
    }

    for (uint64_t index = 0; index < length; ++index) {
        uint64_t function = 0;
        if (!ReadArrayElement(reader, profile, heap_base, class_functions, index, &function) ||
            !RequireCid(reader, function, profile.cid_function)) {
            ++*skipped_function_count;
            continue;
        }
        if (!seen_functions->insert(function).second) continue;

        const uintptr_t function_address = Untag(function);
        uint32_t kind_tag = 0;
        if (!reader.Read(function_address + profile.function_kind_tag_offset, &kind_tag)) {
            ++*skipped_function_count;
            continue;
        }
        const uint32_t kind = kind_tag & 0x1f;
        if (kind == 1 || kind == 2) {
            ++*skipped_function_count;
            continue;
        }

        CollectedLiveFunction collected{};
        collected.info.struct_size = sizeof(collected.info);
        collected.info.function = function;
        collected.info.owner_class = tagged_class;
        collected.info.library = library;
        collected.info.owner_is_toplevel_class = is_top_level ? 1 : 0;
        collected.info.function_kind = kind;
        std::snprintf(collected.info.library_uri, sizeof(collected.info.library_uri), "%s",
                      library_uri);
        std::snprintf(collected.info.class_name, sizeof(collected.info.class_name), "%s",
                      class_name);

        uint64_t tagged_name = 0;
        uint64_t function_owner = 0;
        if (!reader.Read(function_address + profile.function_entry_point_offset,
                         &collected.info.function_entry_point) ||
            !ReadCompressedObject(reader, function_address, profile.function_name_offset, heap_base,
                                  &tagged_name) ||
            !ReadDartString(reader, profile, tagged_name, collected.info.function_name,
                            sizeof(collected.info.function_name)) ||
            !ReadCompressedObject(reader, function_address, profile.function_owner_offset,
                                  heap_base, &function_owner) ||
            function_owner != tagged_class ||
            !ReadCompressedObject(reader, function_address, profile.function_code_offset, heap_base,
                                  &collected.info.code) ||
            !RequireCid(reader, collected.info.code, profile.cid_code)) {
            ++*skipped_function_count;
            continue;
        }

        uint64_t code_owner = 0;
        const uintptr_t code_address = Untag(collected.info.code);
        if (!reader.Read(code_address + profile.code_entry_point_offset,
                         &collected.info.code_entry_point) ||
            !reader.Read(code_address + profile.code_object_pool_offset,
                         &collected.info.code_object_pool) ||
            !reader.Read(code_address + profile.code_owner_offset, &code_owner) ||
            !reader.Read(code_address + profile.code_instructions_length_offset,
                         &collected.info.code_size) ||
            collected.info.function_entry_point == 0 || collected.info.code_size == 0) {
            ++*skipped_function_count;
            continue;
        }
        collected.info.code_owner_matches_function = code_owner == function ? 1 : 0;
        if (!collected.info.code_owner_matches_function &&
            (!RequireCid(reader, code_owner, profile.cid_function) ||
             !HasSnapshotFeature(snapshot.snapshot_features, "dedup_instructions"))) {
            ++*skipped_function_count;
            continue;
        }

        // Express the live entry in the snapshot-instructions coordinate space,
        // rather than assuming ELF VA == runtime - load_bias. This is the same
        // relationship used by FlutterSnapshotSource::ResolveInstructionVa().
        if (snapshot.isolate_instructions_size == 0 ||
            collected.info.function_entry_point < snapshot.isolate_instructions_runtime) {
            ++*skipped_function_count;
            continue;
        }
        const uint64_t instruction_offset =
            collected.info.function_entry_point - snapshot.isolate_instructions_runtime;
        if (instruction_offset >= snapshot.isolate_instructions_size ||
            snapshot.isolate_instructions_va >
                std::numeric_limits<uint64_t>::max() - instruction_offset) {
            ++*skipped_function_count;
            continue;
        }
        collected.info.entry_va = snapshot.isolate_instructions_va + instruction_offset;
        collected.info.code_section_va = snapshot.isolate_instructions_va;
        functions->push_back(collected);
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

    uint64_t num_cids = 0;
    if (!reader.Read(
            static_cast<uintptr_t>(context.class_table) + profile.class_table_num_cids_offset,
            &num_cids) ||
        num_cids == 0 || num_cids > kClassIdTagMask + 1) {
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
        !RequireCid(reader, libraries, profile.cid_growable_object_array)) {
        return false;
    }
    const uintptr_t growable = Untag(libraries);
    uint64_t library_count = 0;
    uint64_t data = 0;
    if (!ReadPositiveCompressedSmi(reader, growable + profile.growable_object_array_length_offset,
                                   &library_count) ||
        library_count > kMaxClassFunctions ||
        !ReadCompressedObject(reader, growable, profile.growable_object_array_data_offset,
                              context.heap_base, &data)) {
        return false;
    }
    for (uint64_t index = 0; index < library_count; ++index) {
        uint64_t library = 0;
        if (!ReadArrayElement(reader, profile, context.heap_base, data, index, &library) ||
            !RequireCid(reader, library, profile.cid_library)) {
            continue;
        }
        uint64_t top_level_class = 0;
        if (!ReadCompressedObject(reader, Untag(library), profile.library_toplevel_class_offset,
                                  context.heap_base, &top_level_class)) {
            continue;
        }
        (void) CollectFunctionsInClass(reader, profile, context.heap_base, top_level_class, true,
                                       snapshot, &seen_functions, functions, &skipped);
    }

    std::unordered_map<uint64_t, uint32_t> aliases;
    for (const CollectedLiveFunction& function : *functions) {
        uint32_t& count = aliases[function.info.function_entry_point];
        if (count != std::numeric_limits<uint32_t>::max()) ++count;
    }
    uint32_t shared_targets = 0;
    for (auto& function : *functions) {
        function.info.entry_alias_count = aliases[function.info.function_entry_point];
        function.info.entry_is_shared = function.info.entry_alias_count > 1 ? 1 : 0;
    }
    for (const auto& [entry, count] : aliases) {
        (void) entry;
        if (count > 1) ++shared_targets;
    }

    out_info->function_count = static_cast<uint32_t>(functions->size());
    out_info->code_target_count = static_cast<uint32_t>(aliases.size());
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
    const DartPlantLiveVmProfile* matched = nullptr;
    for (const DartPlantLiveVmProfile* profile : kLiveVmProfiles) {
        if (profile != nullptr && SameString(snapshot.snapshot_hash, profile->snapshot_hash) &&
            SameString(snapshot.profile_name, profile->snapshot_profile)) {
            matched = profile;
            break;
        }
    }
    if (matched == nullptr) {
        return FailProbe("no exact live VM raw-layout profile for this Dart snapshot");
    }
    *out_profile = *matched;
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

bool DecodeDartType(const ProcessMemoryReader& reader, const FunctionTypeLayout& layout,
                    uint64_t tagged_type, DartPlantDartTypeInfo* out_type) {
    if (out_type == nullptr || !IsHeapObject(tagged_type)) return false;
    DartPlantDartTypeInfo type{};
    type.struct_size = sizeof(type);
    if (!ReadCid(reader, tagged_type, &type.object_cid)) return false;
    if (type.object_cid != layout.cid_type && type.object_cid != layout.cid_function_type &&
        type.object_cid != layout.cid_record_type && type.object_cid != layout.cid_type_parameter) {
        return false;
    }

    uint32_t flags = 0;
    if (!reader.Read(Untag(tagged_type) + layout.abstract_type_flags_offset, &flags) ||
        !DecodeDartNullability(flags, layout, &type.nullability)) {
        return false;
    }

    if (type.object_cid == layout.cid_type) {
        const uint32_t represented_cid =
            static_cast<uint32_t>((flags >> layout.type_class_id_shift) & kClassIdTagMask);
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
        if (!reader.Read(Untag(tagged_type) + layout.type_parameter_base_offset, &base) ||
            !reader.Read(Untag(tagged_type) + layout.type_parameter_index_offset, &index)) {
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
        !RequireCid(reader, tagged_function, profile.cid_function)) {
        return FailProbe("live VM Function is stale or has an invalid CID for signature parsing");
    }

    uint64_t tagged_signature = 0;
    if (!ReadCompressedObject(reader, Untag(tagged_function), layout.function_signature_offset,
                              heap_base, &tagged_signature)) {
        return FailProbe("live VM Function.signature is unreadable");
    }
    uint32_t signature_cid = 0;
    if (!ReadCid(reader, tagged_signature, &signature_cid)) {
        return FailProbe("live VM Function.signature is not a readable heap object");
    }
    if (signature_cid == layout.cid_null) {
        SetLastError("AOT precompiler dropped Function.signature for this function");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (signature_cid != layout.cid_function_type) {
        return FailProbe("live VM Function.signature has an unexpected CID");
    }

    const uintptr_t signature_address = Untag(tagged_signature);
    uint32_t packed_counts = 0;
    uint16_t packed_type_counts = 0;
    uint64_t result_type = 0;
    if (!reader.Read(signature_address + layout.packed_parameter_counts_offset, &packed_counts) ||
        !reader.Read(signature_address + layout.packed_type_parameter_counts_offset,
                     &packed_type_counts) ||
        !ReadCompressedObject(reader, signature_address, layout.result_type_offset, heap_base,
                              &result_type)) {
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
    if (!DecodeDartType(reader, layout, result_type, &parsed.result_type)) {
        return FailProbe("live VM FunctionType result type is invalid");
    }

    if (parsed.parameter_count != 0) {
        if (!ReadCompressedObject(reader, signature_address, layout.parameter_types_offset,
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
        if (!ReadCompressedObject(reader, signature_address, layout.named_parameter_names_offset,
                                  heap_base, &parsed.named_parameter_names)) {
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
        !RequireCid(reader, bool_true, layout->cid) ||
        !RequireCid(reader, bool_false, layout->cid)) {
        SetLastError("Dart canonical Bool roots failed CID validation");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    uint8_t true_value = 0;
    uint8_t false_value = 0xff;
    if (!reader.Read(Untag(bool_true) + layout->value_offset, &true_value) ||
        !reader.Read(Untag(bool_false) + layout->value_offset, &false_value) || true_value != 1 ||
        false_value != 0) {
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
        out_profile->struct_size < sizeof(DartPlantLiveVmProfile)) {
        dartplant::SetLastError("live VM profile selection arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    return dartplant::SelectProfile(*snapshot, out_profile);
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
        registers->null_value != thread_null || !dartplant::IsHeapObject(thread_pool) ||
        context.pp != thread_pool - dartplant::kHeapObjectTag) {
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

    if (!dartplant::RequireCid(reader, context.global_object_pool, profile.cid_object_pool) ||
        !reader.Read(
            dartplant::Untag(context.global_object_pool) + profile.object_pool_length_offset,
            &context.object_pool_length) ||
        context.object_pool_length == 0 ||
        context.object_pool_length > dartplant::kMaxObjectPoolEntries) {
        return dartplant::FailProbe("sampled global ObjectPool is invalid");
    }

    uint64_t num_cids = 0;
    if (!reader.Read(
            static_cast<uintptr_t>(context.class_table) + profile.class_table_num_cids_offset,
            &num_cids) ||
        num_cids == 0 || num_cids > dartplant::kClassIdTagMask + 1) {
        return dartplant::FailProbe("sampled ClassTable is invalid");
    }
    uint64_t class_class = 0;
    if (profile.cid_class >= num_cids ||
        !reader.Read(static_cast<uintptr_t>(context.cached_class_table_table) +
                         static_cast<uintptr_t>(profile.cid_class) * sizeof(uint64_t),
                     &class_class) ||
        !dartplant::RequireCid(reader, class_class, profile.cid_class)) {
        return dartplant::FailProbe("sampled cached ClassTable does not expose Class CID");
    }

    uint64_t libraries = 0;
    if (!reader.Read(
            static_cast<uintptr_t>(context.object_store) + profile.object_store_libraries_offset,
            &libraries) ||
        !dartplant::RequireCid(reader, libraries, profile.cid_growable_object_array)) {
        return dartplant::FailProbe("sampled ObjectStore.libraries is invalid");
    }
    const uintptr_t growable = dartplant::Untag(libraries);
    uint64_t library_count = 0;
    uint64_t library_data = 0;
    if (!dartplant::ReadPositiveCompressedSmi(
            reader, growable + profile.growable_object_array_length_offset, &library_count) ||
        library_count == 0 || library_count > dartplant::kMaxClassFunctions ||
        !dartplant::ReadCompressedObject(reader, growable,
                                         profile.growable_object_array_data_offset,
                                         context.heap_base, &library_data)) {
        return dartplant::FailProbe("sampled ObjectStore library array is invalid");
    }
    uint32_t library_data_cid = 0;
    if (!dartplant::ReadCid(reader, library_data, &library_data_cid) ||
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
            !dartplant::RequireCid(reader, library, profile.cid_library)) {
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
        out_info->struct_size < sizeof(DartPlantLiveVmProbeInfo)) {
        dartplant::SetLastError("live VM probe arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    DartPlantStatus status = dartplant::SelectProfile(*snapshot, &profile);
    if (status != DARTPLANT_OK) return status;

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
    info.thread_pool_match =
        dartplant::IsHeapObject(thread_pool) && info.pp == thread_pool - dartplant::kHeapObjectTag
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

    if (!dartplant::RequireCid(reader, info.global_object_pool, profile.cid_object_pool)) {
        return dartplant::FailProbe("THR global object pool does not have ObjectPool CID");
    }
    if (!reader.Read(dartplant::Untag(info.global_object_pool) + profile.object_pool_length_offset,
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
    uint64_t indexed_function = 0;
    if (!dartplant::FindFunctionByEntryIdentity(
            reader, profile, info.heap_base, static_cast<uintptr_t>(info.class_table),
            static_cast<uintptr_t>(info.cached_class_table_table),
            static_cast<uintptr_t>(info.object_store), target_entry, expected_function,
            expected_class, &indexed_function, &info.entry_alias_count)) {
        return dartplant::FailProbe(
            "ClassTable/ObjectStore live index could not resolve current method identity");
    }
    info.function = indexed_function;
    info.function_found_from_vm_index = 1;
    info.entry_is_shared = info.entry_alias_count > 1 ? 1 : 0;
    if (!dartplant::RequireCid(reader, info.function, profile.cid_function)) {
        return dartplant::FailProbe("live VM index returned a non-Function object");
    }

    const uintptr_t function = dartplant::Untag(info.function);
    uint64_t function_name = 0;
    uint64_t function_owner = 0;
    uint64_t function_code = 0;
    if (!reader.Read(function + profile.function_entry_point_offset, &info.function_entry_point) ||
        !dartplant::ReadCompressedObject(reader, function, profile.function_name_offset,
                                         info.heap_base, &function_name) ||
        !dartplant::ReadCompressedObject(reader, function, profile.function_owner_offset,
                                         info.heap_base, &function_owner) ||
        !dartplant::ReadCompressedObject(reader, function, profile.function_code_offset,
                                         info.heap_base, &function_code)) {
        return dartplant::FailProbe("failed to read Dart Function raw fields");
    }
    if (info.function_entry_point != target_entry ||
        !dartplant::ReadDartString(reader, profile, function_name, info.function_name,
                                   sizeof(info.function_name))) {
        return dartplant::FailProbe("Function entry/name semantic validation failed");
    }

    info.code = function_code;
    if (!dartplant::RequireCid(reader, info.code, profile.cid_code)) {
        return dartplant::FailProbe("Function.code is not a Dart Code object");
    }
    uint64_t code_pool = 0;
    if (!reader.Read(dartplant::Untag(info.code) + profile.code_entry_point_offset,
                     &info.code_entry_point) ||
        !reader.Read(dartplant::Untag(info.code) + profile.code_object_pool_offset, &code_pool) ||
        !reader.Read(dartplant::Untag(info.code) + profile.code_owner_offset, &info.code_owner)) {
        return dartplant::FailProbe("failed to read Dart Code raw fields");
    }
    info.function_code_match = info.code_owner == info.function ? 1 : 0;
    info.code_owner_is_function =
        dartplant::RequireCid(reader, info.code_owner, profile.cid_function) ? 1 : 0;
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
    info.code_pool_match = info.pp == info.global_object_pool - dartplant::kHeapObjectTag &&
                                   (info.code_pool_is_null || code_pool == info.global_object_pool)
                               ? 1
                               : 0;
    if (!info.code_pool_match) {
        return dartplant::FailProbe(
            "Code effective ObjectPool does not match PP/global ObjectPool");
    }

    info.owner_class = function_owner;
    if (!dartplant::RequireCid(reader, info.owner_class, profile.cid_class)) {
        return dartplant::FailProbe("Function.owner is not a Dart Class object");
    }
    const uintptr_t owner_class = dartplant::Untag(info.owner_class);
    uint64_t class_name = 0;
    uint64_t class_functions = 0;
    if (!dartplant::ReadCompressedObject(reader, owner_class, profile.class_name_offset,
                                         info.heap_base, &class_name) ||
        !dartplant::ReadCompressedObject(reader, owner_class, profile.class_functions_offset,
                                         info.heap_base, &class_functions) ||
        !dartplant::ReadCompressedObject(reader, owner_class, profile.class_library_offset,
                                         info.heap_base, &info.library)) {
        return dartplant::FailProbe("failed to read Dart Class raw fields");
    }
    if (!dartplant::ReadDartString(reader, profile, class_name, info.class_name,
                                   sizeof(info.class_name)) ||
        !dartplant::RequireCid(reader, info.library, profile.cid_library)) {
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

    const uintptr_t library = dartplant::Untag(info.library);
    uint64_t library_url = 0;
    uint64_t top_level_class = 0;
    if (!dartplant::ReadCompressedObject(reader, library, profile.library_url_offset,
                                         info.heap_base, &library_url) ||
        !dartplant::ReadCompressedObject(reader, library, profile.library_toplevel_class_offset,
                                         info.heap_base, &top_level_class) ||
        !dartplant::ReadDartString(reader, profile, library_url, info.library_uri,
                                   sizeof(info.library_uri))) {
        return dartplant::FailProbe("failed to reconstruct Dart Library URL/top-level class");
    }
    info.owner_is_toplevel_class = top_level_class == info.owner_class ? 1 : 0;
    if (info.owner_is_toplevel_class) {
        std::snprintf(info.class_name, sizeof(info.class_name), "%s", "Global");
    }

    *out_info = info;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_live_vm_context_from_probe(
    const DartPlantLiveVmProbeInfo* probe, DartPlantLiveVmContext* out_context) {
    if (probe == nullptr || out_context == nullptr ||
        probe->struct_size < sizeof(DartPlantLiveVmProbeInfo) ||
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
        !dartplant::RequireCid(reader, context->global_object_pool, profile.cid_object_pool)) {
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

    const uintptr_t function_address = dartplant::Untag(function);
    uint64_t tagged_name = 0;
    uint64_t function_owner = 0;
    if (!reader.Read(function_address + profile.function_entry_point_offset,
                     &method.function_entry_point) ||
        !dartplant::ReadCompressedObject(reader, function_address, profile.function_name_offset,
                                         context->heap_base, &tagged_name) ||
        !dartplant::ReadCompressedObject(reader, function_address, profile.function_owner_offset,
                                         context->heap_base, &function_owner) ||
        !dartplant::ReadCompressedObject(reader, function_address, profile.function_code_offset,
                                         context->heap_base, &method.code) ||
        !dartplant::ReadDartString(reader, profile, tagged_name, method.function_name,
                                   sizeof(method.function_name))) {
        return dartplant::FailProbe("failed to reconstruct live VM Function identity");
    }

    method.owner_class = function_owner;
    if (!dartplant::RequireCid(reader, method.owner_class, profile.cid_class) ||
        !dartplant::RequireCid(reader, method.code, profile.cid_code)) {
        return dartplant::FailProbe("live VM Function owner or Code has an invalid CID");
    }

    uint64_t class_name_object = 0;
    uint64_t class_functions = 0;
    uint64_t class_library = 0;
    if (!dartplant::ReadCompressedObject(reader, dartplant::Untag(method.owner_class),
                                         profile.class_name_offset, context->heap_base,
                                         &class_name_object) ||
        !dartplant::ReadCompressedObject(reader, dartplant::Untag(method.owner_class),
                                         profile.class_functions_offset, context->heap_base,
                                         &class_functions) ||
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
    if (!dartplant::ReadCompressedObject(reader, dartplant::Untag(method.library),
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
    if (!reader.Read(dartplant::Untag(method.code) + profile.code_entry_point_offset,
                     &method.code_entry_point) ||
        !reader.Read(dartplant::Untag(method.code) + profile.code_object_pool_offset, &code_pool) ||
        !reader.Read(dartplant::Untag(method.code) + profile.code_owner_offset,
                     &method.code_owner) ||
        !reader.Read(dartplant::Untag(method.code) + profile.code_instructions_length_offset,
                     &method.code_size) ||
        method.code_size == 0) {
        return dartplant::FailProbe("failed to reconstruct live VM Code");
    }
    method.function_code_owner_match = method.code_owner == method.function ? 1 : 0;
    method.code_owner_is_function =
        dartplant::RequireCid(reader, method.code_owner, profile.cid_function) ? 1 : 0;
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
            static_cast<uintptr_t>(method.function_entry_point), function_name, class_name,
            &ignored_function, &method.entry_alias_count)) {
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
    if (!dartplant::DecodeDartType(reader, *layout, tagged_type, &parameter.type)) {
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
            uint32_t raw_flags = 0;
            if (!dartplant::ReadArrayRawElement(reader, profile, parsed.named_parameter_names,
                                                flag_index, &raw_flags) ||
                (raw_flags & dartplant::kSmiTagMask) != 0) {
                return dartplant::FailProbe("FunctionType required-named flags are not a Smi");
            }
            const uint32_t flags = raw_flags >> 1;
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
    if (!reader.Refresh() ||
        !dartplant::RequireCid(reader, tagged_object_pool, profile.cid_object_pool)) {
        return dartplant::FailProbe("ObjectPool is stale or has an invalid CID");
    }
    const uintptr_t pool = dartplant::Untag(tagged_object_pool);
    uint64_t length = 0;
    if (!reader.Read(pool + profile.object_pool_length_offset, &length) ||
        length > dartplant::kMaxObjectPoolEntries || index >= length) {
        dartplant::SetLastError("ObjectPool index is out of range");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    const uintptr_t data_start = pool + profile.object_pool_elements_offset;
    uint64_t raw = 0;
    uint8_t bits = 0;
    if (!reader.Read(data_start + static_cast<uintptr_t>(index) * sizeof(uint64_t), &raw) ||
        !reader.Read(data_start + static_cast<uintptr_t>(length) * sizeof(uint64_t) + index,
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
        if (dartplant::IsHeapObject(raw)) {
            (void) dartplant::ReadCid(reader, raw, &entry.object_cid);
        }
    }
    *out_entry = entry;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}
