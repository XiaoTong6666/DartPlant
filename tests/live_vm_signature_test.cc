// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "dartplant/advanced/live_vm.h"
#include "test_runner.h"

namespace {

constexpr uint32_t kClassIdTagShift = 12;
constexpr const char* kSnapshotProfile = "flutter-arm64-product-compressed";

struct SignatureProfileCase {
    const char* snapshot_hash;
    uint32_t profile_version;
    uint32_t cid_type;
    uint32_t cid_function_type;
    uint32_t cid_record_type;
    uint32_t cid_type_parameter;
    uint32_t cid_null;
    uint32_t cid_dynamic;
    uint32_t cid_void;
    uint32_t cid_never;
    uint32_t cid_bool;
    uint8_t nullability_bits;
    uint8_t type_class_id_shift;
};

constexpr std::array<SignatureProfileCase, 3> kSignatureProfiles = {{
    {"d20a1be77c3d3c41b2a5accaee1ce549", 1, 48, 49, 50, 51, 170, 171, 172, 173, 62, 2, 4},
    {"80a49c7111088100a233b2ae788e1f48", 2, 48, 49, 50, 51, 170, 171, 172, 173, 62, 1, 3},
    {"ace654289f5abc240509fc941453ebc5", 3, 49, 50, 51, 52, 171, 172, 173, 174, 63, 1, 3},
}};

class SyntheticDartHeap final {
public:
    uint64_t base() const { return reinterpret_cast<uint64_t>(bytes_.data()); }
    uint64_t Tagged(size_t offset) const { return base() + offset + 1; }

    template <typename T>
    void Store(size_t offset, T value) {
        std::memcpy(bytes_.data() + offset, &value, sizeof(value));
    }

    void SetCid(size_t object_offset, uint32_t cid) {
        Store<uint64_t>(object_offset, static_cast<uint64_t>(cid) << kClassIdTagShift);
    }

    void SetCompressedPointer(size_t object_offset, uint32_t field_offset, size_t target_offset) {
        Store<uint32_t>(object_offset + field_offset, static_cast<uint32_t>(target_offset + 1));
    }

    void SetCompressedSmi(size_t address, uint32_t value) { Store<uint32_t>(address, value << 1); }

    void SetType(size_t object_offset, uint32_t object_cid, uint32_t type_class_id,
                 uint8_t nullability, uint8_t type_class_id_shift) {
        SetCid(object_offset, object_cid);
        const uint32_t flags = (type_class_id << type_class_id_shift) | nullability;
        Store<uint32_t>(object_offset + 0x10, flags);
    }

    void SetAbstractType(size_t object_offset, uint32_t object_cid, uint8_t nullability) {
        SetCid(object_offset, object_cid);
        Store<uint32_t>(object_offset + 0x10, nullability);
    }

    void SetTypeParameter(size_t object_offset, uint32_t object_cid, uint8_t nullability,
                          uint8_t function_bit, uint16_t base, uint16_t index) {
        SetCid(object_offset, object_cid);
        Store<uint32_t>(object_offset + 0x10,
                        static_cast<uint32_t>(nullability) | (1U << function_bit));
        Store<uint16_t>(object_offset + 0x24, base);
        Store<uint16_t>(object_offset + 0x26, index);
    }

    void SetArray(size_t object_offset, uint32_t array_cid, uint32_t length) {
        SetCid(object_offset, array_cid);
        SetCompressedSmi(object_offset + 0x0c, length);
    }

    void SetArrayPointer(size_t object_offset, uint32_t index, size_t target_offset) {
        Store<uint32_t>(object_offset + 0x10 + index * sizeof(uint32_t),
                        static_cast<uint32_t>(target_offset + 1));
    }

    void SetArraySmi(size_t object_offset, uint32_t index, uint32_t value) {
        Store<uint32_t>(object_offset + 0x10 + index * sizeof(uint32_t), value << 1);
    }

    void SetOneByteString(size_t object_offset, uint32_t string_cid, std::string_view value) {
        SetCid(object_offset, string_cid);
        SetCompressedSmi(object_offset + 0x08, static_cast<uint32_t>(value.size()));
        std::memcpy(bytes_.data() + object_offset + 0x10, value.data(), value.size());
    }

private:
    alignas(16) std::array<uint8_t, 4096> bytes_{};
};

struct SyntheticSignatureFixture {
    SyntheticDartHeap heap;
    DartPlantFlutterSnapshotInfo snapshot{};
    DartPlantLiveVmProfile profile{};
    DartPlantLiveVmContext context{};
    uint64_t function = 0;
    size_t function_offset = 0x100;
    size_t signature_offset = 0x200;
    size_t null_object_offset = 0xe00;
};

SyntheticSignatureFixture BuildSignatureFixture(const SignatureProfileCase& item) {
    SyntheticSignatureFixture fixture;
    fixture.snapshot.struct_size = sizeof(fixture.snapshot);
    fixture.snapshot.snapshot_hash = item.snapshot_hash;
    fixture.snapshot.profile_name = kSnapshotProfile;
    fixture.snapshot.compressed_pointers = 1;

    fixture.profile.struct_size = sizeof(fixture.profile);
    EXPECT_EQ(DARTPLANT_OK, dartplant_live_vm_select_profile(&fixture.snapshot, &fixture.profile));
    EXPECT_EQ(item.profile_version, fixture.profile.profile_version);

    fixture.context.struct_size = sizeof(fixture.context);
    fixture.context.profile_version = fixture.profile.profile_version;
    fixture.context.profile_name = fixture.profile.name;
    fixture.context.heap_base = fixture.heap.base();

    constexpr size_t kParameterTypes = 0x300;
    constexpr size_t kNamedNames = 0x380;
    constexpr size_t kResultType = 0x500;
    constexpr std::array<size_t, 8> kParameterTypeOffsets = {
        0x580, 0x600, 0x680, 0x700, 0x780, 0x800, 0x880, 0x900,
    };
    constexpr std::array<size_t, 6> kNameOffsets = {0xa00, 0xa40, 0xa80, 0xac0, 0xb00, 0xb40};
    constexpr std::array<std::string_view, 6> kNames = {"fn", "tp", "nil", "v", "n", "rec"};

    fixture.heap.SetCid(fixture.function_offset, fixture.profile.cid_function);
    fixture.heap.SetCompressedPointer(fixture.function_offset, 0x20, fixture.signature_offset);
    fixture.function = fixture.heap.Tagged(fixture.function_offset);

    fixture.heap.SetAbstractType(fixture.signature_offset, item.cid_function_type, 1);
    fixture.heap.SetCompressedPointer(fixture.signature_offset, 0x24, kResultType);
    fixture.heap.SetCompressedPointer(fixture.signature_offset, 0x28, kParameterTypes);
    fixture.heap.SetCompressedPointer(fixture.signature_offset, 0x2c, kNamedNames);
    const uint32_t packed_counts = 1U | (1U << 1) | (2U << 2) | (6U << 16);
    fixture.heap.Store<uint32_t>(fixture.signature_offset + 0x30, packed_counts);
    fixture.heap.Store<uint16_t>(fixture.signature_offset + 0x34, static_cast<uint16_t>(0x0302));

    fixture.heap.SetType(kResultType, item.cid_type, item.cid_bool, 1, item.type_class_id_shift);
    fixture.heap.SetType(kParameterTypeOffsets[0], item.cid_type, 120, 1, item.type_class_id_shift);
    fixture.heap.SetType(kParameterTypeOffsets[1], item.cid_type, item.cid_dynamic, 0,
                         item.type_class_id_shift);
    fixture.heap.SetAbstractType(kParameterTypeOffsets[2], item.cid_function_type, 1);
    fixture.heap.SetTypeParameter(kParameterTypeOffsets[3], item.cid_type_parameter,
                                  item.nullability_bits == 2 ? 2 : 0, item.type_class_id_shift, 2,
                                  5);
    fixture.heap.SetType(kParameterTypeOffsets[4], item.cid_type, item.cid_null, 0,
                         item.type_class_id_shift);
    fixture.heap.SetType(kParameterTypeOffsets[5], item.cid_type, item.cid_void, 0,
                         item.type_class_id_shift);
    fixture.heap.SetType(kParameterTypeOffsets[6], item.cid_type, item.cid_never, 1,
                         item.type_class_id_shift);
    fixture.heap.SetAbstractType(kParameterTypeOffsets[7], item.cid_record_type, 1);

    fixture.heap.SetArray(kParameterTypes, fixture.profile.cid_array, 8);
    for (uint32_t index = 0; index < kParameterTypeOffsets.size(); ++index) {
        fixture.heap.SetArrayPointer(kParameterTypes, index, kParameterTypeOffsets[index]);
    }

    fixture.heap.SetArray(kNamedNames, fixture.profile.cid_array, 7);
    for (uint32_t index = 0; index < kNameOffsets.size(); ++index) {
        fixture.heap.SetOneByteString(kNameOffsets[index], fixture.profile.cid_one_byte_string,
                                      kNames[index]);
        fixture.heap.SetArrayPointer(kNamedNames, index, kNameOffsets[index]);
    }
    // Named indexes 0, 2 and 4 are required. The VM appends this bitmap as a Smi.
    fixture.heap.SetArraySmi(kNamedNames, 6, 0b010101);

    fixture.heap.SetCid(fixture.null_object_offset, item.cid_null);
    return fixture;
}

}  // namespace

TEST_CASE(LiveVmParsesRetainedFunctionTypeAcrossSupportedProfiles) {
    for (const auto& item : kSignatureProfiles) {
        auto fixture = BuildSignatureFixture(item);

        DartPlantDartFunctionSignatureInfo signature{};
        signature.struct_size = sizeof(signature);
        EXPECT_EQ(DARTPLANT_OK,
                  dartplant_live_vm_read_function_signature(&fixture.context, &fixture.snapshot,
                                                            fixture.function, &signature));
        EXPECT_EQ(8U, signature.parameter_count);
        EXPECT_EQ(1U, signature.implicit_parameter_count);
        EXPECT_EQ(2U, signature.fixed_parameter_count);
        EXPECT_EQ(6U, signature.optional_parameter_count);
        EXPECT_EQ(3U, signature.type_parameter_count);
        EXPECT_EQ(2U, signature.parent_type_argument_count);
        EXPECT_EQ(1U, signature.has_named_optional_parameters);
        EXPECT_EQ(DARTPLANT_DART_TYPE_INTERFACE, signature.result_type.kind);
        EXPECT_EQ(item.cid_bool, signature.result_type.type_class_id);
        EXPECT_EQ(DARTPLANT_DART_NULLABILITY_NON_NULLABLE, signature.result_type.nullability);

        const std::array<DartPlantDartTypeKind, 8> expected_types = {
            DARTPLANT_DART_TYPE_INTERFACE, DARTPLANT_DART_TYPE_DYNAMIC,
            DARTPLANT_DART_TYPE_FUNCTION,  DARTPLANT_DART_TYPE_PARAMETER,
            DARTPLANT_DART_TYPE_NULL,      DARTPLANT_DART_TYPE_VOID,
            DARTPLANT_DART_TYPE_NEVER,     DARTPLANT_DART_TYPE_RECORD,
        };
        for (uint32_t index = 0; index < expected_types.size(); ++index) {
            DartPlantDartParameterInfo parameter{};
            parameter.struct_size = sizeof(parameter);
            EXPECT_EQ(DARTPLANT_OK, dartplant_live_vm_read_function_parameter(
                                        &fixture.context, &fixture.snapshot, fixture.function,
                                        index, &parameter));
            EXPECT_EQ(index, parameter.index);
            EXPECT_EQ(expected_types[index], parameter.type.kind);
            if (index == 0) {
                EXPECT_EQ(DARTPLANT_DART_PARAMETER_IMPLICIT, parameter.kind);
                EXPECT_EQ(0U, parameter.is_required);
                EXPECT_EQ(std::string_view(), std::string_view(parameter.name));
            } else if (index == 1) {
                EXPECT_EQ(DARTPLANT_DART_PARAMETER_REQUIRED_POSITIONAL, parameter.kind);
                EXPECT_EQ(1U, parameter.is_required);
                EXPECT_EQ(std::string_view(), std::string_view(parameter.name));
            } else {
                EXPECT_EQ(DARTPLANT_DART_PARAMETER_NAMED, parameter.kind);
                EXPECT_EQ(static_cast<uint8_t>(index == 2 || index == 4 || index == 6),
                          parameter.is_required);
            }
        }

        DartPlantDartParameterInfo type_parameter{};
        type_parameter.struct_size = sizeof(type_parameter);
        EXPECT_EQ(DARTPLANT_OK,
                  dartplant_live_vm_read_function_parameter(&fixture.context, &fixture.snapshot,
                                                            fixture.function, 3, &type_parameter));
        EXPECT_EQ(item.nullability_bits == 2 ? DARTPLANT_DART_NULLABILITY_LEGACY
                                             : DARTPLANT_DART_NULLABILITY_NULLABLE,
                  type_parameter.type.nullability);
        EXPECT_EQ(2U, type_parameter.type.type_parameter_base);
        EXPECT_EQ(5U, type_parameter.type.type_parameter_index);
        EXPECT_EQ(1U, type_parameter.type.is_function_type_parameter);

        DartPlantDartParameterInfo out_of_range{};
        out_of_range.struct_size = sizeof(out_of_range);
        EXPECT_EQ(DARTPLANT_INVALID_ARGUMENT,
                  dartplant_live_vm_read_function_parameter(&fixture.context, &fixture.snapshot,
                                                            fixture.function, 8, &out_of_range));
    }
}

TEST_CASE(LiveVmParsesOptionalPositionalParameters) {
    for (const auto& item : kSignatureProfiles) {
        auto fixture = BuildSignatureFixture(item);
        const uint32_t packed_counts = 1U | (2U << 2) | (6U << 16);
        fixture.heap.Store<uint32_t>(fixture.signature_offset + 0x30, packed_counts);

        DartPlantDartFunctionSignatureInfo signature{};
        signature.struct_size = sizeof(signature);
        EXPECT_EQ(DARTPLANT_OK,
                  dartplant_live_vm_read_function_signature(&fixture.context, &fixture.snapshot,
                                                            fixture.function, &signature));
        EXPECT_EQ(0U, signature.has_named_optional_parameters);

        DartPlantDartParameterInfo parameter{};
        parameter.struct_size = sizeof(parameter);
        EXPECT_EQ(DARTPLANT_OK,
                  dartplant_live_vm_read_function_parameter(&fixture.context, &fixture.snapshot,
                                                            fixture.function, 2, &parameter));
        EXPECT_EQ(DARTPLANT_DART_PARAMETER_OPTIONAL_POSITIONAL, parameter.kind);
        EXPECT_EQ(0U, parameter.is_required);
        EXPECT_EQ(std::string_view(), std::string_view(parameter.name));
    }
}

TEST_CASE(LiveVmFunctionTypeRejectsMissingNamedParameterFlagSlots) {
    for (const auto& item : kSignatureProfiles) {
        auto fixture = BuildSignatureFixture(item);
        // Six named parameters require six name slots plus one ARM64 Smi flag
        // slot. Truncating the array to the names alone must fail closed rather
        // than silently treating every named parameter as optional.
        fixture.heap.SetArray(0x380, fixture.profile.cid_array, 6);

        DartPlantDartFunctionSignatureInfo signature{};
        signature.struct_size = sizeof(signature);
        EXPECT_EQ(DARTPLANT_PROFILE_MISMATCH,
                  dartplant_live_vm_read_function_signature(&fixture.context, &fixture.snapshot,
                                                            fixture.function, &signature));
    }
}

TEST_CASE(LiveVmFunctionTypeFailsClosedWhenAotDroppedSignature) {
    for (const auto& item : kSignatureProfiles) {
        auto fixture = BuildSignatureFixture(item);
        fixture.heap.SetCompressedPointer(fixture.function_offset, 0x20,
                                          fixture.null_object_offset);

        DartPlantDartFunctionSignatureInfo signature{};
        signature.struct_size = sizeof(signature);
        EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY,
                  dartplant_live_vm_read_function_signature(&fixture.context, &fixture.snapshot,
                                                            fixture.function, &signature));
    }
}
