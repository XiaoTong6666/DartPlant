// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_VM_RUNTIME_PROFILES_H_
#define DARTPLANT_VM_RUNTIME_PROFILES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "dartplant/advanced/live_vm.h"

namespace dartplant {

struct CanonicalBoolLayout {
    uint32_t thread_true_offset;
    uint32_t thread_false_offset;
    uint32_t value_offset;
    uint32_t cid;
};

struct FunctionTypeLayout {
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

struct RuntimeProfileRecord {
    DartPlantLiveVmProfile live_vm;
    uint8_t dart_sp_register;
    uint8_t arguments_descriptor_register;
    std::array<uint8_t, 6> dart_gp_argument_registers;
    std::array<uint8_t, 6> dart_fpu_argument_registers;
    uint32_t instructions_monomorphic_entry_offset_aot;
    uint32_t instructions_polymorphic_entry_offset_aot;
    uint32_t thread_jump_to_frame_entry_point_offset;
    CanonicalBoolLayout canonical_bool;
    FunctionTypeLayout function_type;
};

struct AotCodePayloadRange {
    uint64_t start = 0;
    uint64_t end = 0;
    bool has_monomorphic_entry = false;
};

const RuntimeProfileRecord* RuntimeProfiles();
size_t RuntimeProfileCount();
const RuntimeProfileRecord* FindRuntimeProfileByVersion(uint32_t profile_version);
const RuntimeProfileRecord* FindRuntimeProfileBySnapshot(std::string_view snapshot_hash,
                                                         std::string_view snapshot_profile = {});
uint32_t ThreadJumpToFrameOffsetForSnapshot(std::string_view snapshot_hash);
bool ComputeAotCodePayloadStart(uint32_t profile_version, uint64_t normal_entry,
                                uint64_t monomorphic_entry, uint64_t* out_start,
                                bool* out_has_monomorphic_entry = nullptr);
bool ComputeAotCodePayloadRange(uint32_t profile_version, uint64_t normal_entry,
                                uint64_t monomorphic_entry, uint32_t instructions_length,
                                AotCodePayloadRange* out_range);

}  // namespace dartplant

#endif  // DARTPLANT_VM_RUNTIME_PROFILES_H_
