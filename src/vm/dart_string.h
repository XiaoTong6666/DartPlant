#ifndef DARTPLANT_VM_DART_STRING_H_
#define DARTPLANT_VM_DART_STRING_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "vm/runtime_profiles.h"

namespace dartplant::vm_abi {

// Read a Dart String through a caller-owned memory reader and encode either
// OneByteString or UTF-16 TwoByteString content as bounded UTF-8.
template <typename ReadMemory>
bool ReadDartStringUtf8(const RuntimeProfileRecord& profile, uint64_t tagged,
                        ReadMemory&& read_memory, char* output, size_t capacity) {
    const auto& raw = profile.raw_object;
    const auto& vm = profile.live_vm;
    if (output == nullptr || capacity == 0 || raw.compressed_word_size != sizeof(uint32_t) ||
        (tagged & raw.smi_tag_mask) != raw.heap_object_tag || tagged < raw.heap_object_tag ||
        raw.class_id_tag_bits == 0 || raw.class_id_tag_bits >= 64) {
        return false;
    }

    const uintptr_t object = static_cast<uintptr_t>(tagged - raw.heap_object_tag);
    uint64_t tags = 0;
    if (!read_memory(object, &tags, sizeof(tags))) return false;
    const uint64_t class_id_mask = (uint64_t{1} << raw.class_id_tag_bits) - 1;
    const uint32_t cid = static_cast<uint32_t>((tags >> raw.class_id_tag_shift) & class_id_mask);
    if (cid != vm.cid_one_byte_string && cid != vm.cid_two_byte_string) return false;

    uint32_t raw_length = 0;
    if (!read_memory(object + vm.string_length_offset, &raw_length, sizeof(raw_length)) ||
        (raw_length & raw.smi_tag_mask) != raw.smi_tag || raw.smi_tag_shift >= 32) {
        return false;
    }
    const size_t length = static_cast<size_t>(raw_length >> raw.smi_tag_shift);
    if (length > (1U << 20)) return false;

    output[0] = '\0';
    size_t cursor = 0;
    const auto append = [&](uint32_t code_point) {
        size_t encoded_size = 0;
        if (code_point <= 0x7f) {
            encoded_size = 1;
        } else if (code_point <= 0x7ff) {
            encoded_size = 2;
        } else if (code_point <= 0xffff) {
            encoded_size = 3;
        } else if (code_point <= 0x10ffff) {
            encoded_size = 4;
        } else {
            return false;
        }
        if (cursor >= capacity || encoded_size >= capacity - cursor) return false;
        if (encoded_size == 1) {
            output[cursor++] = static_cast<char>(code_point);
        } else if (encoded_size == 2) {
            output[cursor++] = static_cast<char>(0xc0 | (code_point >> 6));
            output[cursor++] = static_cast<char>(0x80 | (code_point & 0x3f));
        } else if (encoded_size == 3) {
            output[cursor++] = static_cast<char>(0xe0 | (code_point >> 12));
            output[cursor++] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
            output[cursor++] = static_cast<char>(0x80 | (code_point & 0x3f));
        } else {
            output[cursor++] = static_cast<char>(0xf0 | (code_point >> 18));
            output[cursor++] = static_cast<char>(0x80 | ((code_point >> 12) & 0x3f));
            output[cursor++] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
            output[cursor++] = static_cast<char>(0x80 | (code_point & 0x3f));
        }
        return true;
    };

    if (cid == vm.cid_one_byte_string) {
        for (size_t index = 0; index < length; ++index) {
            uint8_t byte = 0;
            if (!read_memory(object + vm.string_data_offset + index, &byte, sizeof(byte)) ||
                !append(byte)) {
                return false;
            }
        }
    } else {
        if (length > std::numeric_limits<size_t>::max() / sizeof(uint16_t)) return false;
        std::vector<uint16_t> units(length);
        if (length != 0 &&
            !read_memory(object + vm.string_data_offset, units.data(), length * sizeof(uint16_t))) {
            return false;
        }
        for (size_t index = 0; index < units.size(); ++index) {
            const uint16_t unit = units[index];
            uint32_t code_point = unit;
            if (unit >= 0xd800 && unit <= 0xdbff) {
                if (index + 1 >= units.size() || units[index + 1] < 0xdc00 ||
                    units[index + 1] > 0xdfff) {
                    return false;
                }
                code_point = 0x10000 + ((static_cast<uint32_t>(unit) - 0xd800) << 10) +
                             (units[++index] - 0xdc00);
            } else if (unit >= 0xdc00 && unit <= 0xdfff) {
                return false;
            }
            if (!append(code_point)) return false;
        }
    }
    output[cursor] = '\0';
    return true;
}

}  // namespace dartplant::vm_abi

#endif  // DARTPLANT_VM_DART_STRING_H_
