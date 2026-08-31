// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "abi/calling_convention.h"

#include <algorithm>
#include <limits>

namespace dartplant::abi {
namespace {

bool IsProven(const DartAbiResolvedSlot& slot) {
    return slot.proof == DartAbiProofState::kProven &&
           slot.representation != DartAbiRepresentation::kUnknown;
}

bool IsSupportedParameterRepresentation(DartAbiRepresentation representation) {
    return representation == DartAbiRepresentation::kTagged ||
           representation == DartAbiRepresentation::kUnboxedInt64 ||
           representation == DartAbiRepresentation::kUnboxedDouble;
}

bool IsSupportedResultRepresentation(DartAbiRepresentation representation) {
    return IsSupportedParameterRepresentation(representation) ||
           representation == DartAbiRepresentation::kPairOfTagged;
}

DartAbiLocation Gp(uint8_t index) {
    return {.kind = DartAbiLocationKind::kGpRegister, .register_index = index};
}

DartAbiLocation Fpu(uint8_t index) {
    return {.kind = DartAbiLocationKind::kFpuRegister, .register_index = index};
}

DartAbiLocation EntryStack(int32_t slot, uint32_t word_size) {
    return {
        .kind = DartAbiLocationKind::kEntryStack,
        .stack_offset = slot * static_cast<int32_t>(word_size),
    };
}

bool ValidateProfile(const DartCallingConventionProfile& profile) {
    return profile.word_size != 0 &&
           profile.word_size <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) &&
           profile.gp_argument_register_count <= profile.gp_argument_registers.size() &&
           profile.fpu_argument_register_count <= profile.fpu_argument_registers.size();
}

}  // namespace

DartCallingConventionProfile Arm64AotCallingConventionProfile() {
    DartCallingConventionProfile profile;
    profile.word_size = 8;
    profile.dart_sp_register = 15;
    profile.gp_argument_registers = {1, 2, 3, 5, 6, 7, 0, 0};
    profile.gp_argument_register_count = 6;
    profile.fpu_argument_registers = {0, 1, 2, 3, 4, 5, 0, 0};
    profile.fpu_argument_register_count = 6;
    profile.tagged_result_gp_register = 0;
    profile.unboxed_int64_result_gp_register = 0;
    profile.unboxed_double_result_fpu_register = 0;
    profile.pair_of_tagged_result_gp_registers = {0, 1};
    // stack_frame_arm64.h: kParamEndSlotFromFp=1,
    // kLastParamSlotFromEntrySp=0. locations.cc computes:
    //   (param_end_from_fp + 1) - last_param_from_entry_sp
    profile.fp_to_entry_sp_slot_delta = 2;
    return profile;
}

DartCallLayoutStatus ComputeDartCallLayout(const DartFunctionAbiResolution& abi,
                                           const DartCallingConventionProfile& profile,
                                           DartCallLayout* out_layout, bool closure_call) {
    if (out_layout == nullptr || !ValidateProfile(profile)) {
        return DartCallLayoutStatus::kInvalidCallingConvention;
    }
    *out_layout = {};

    if (abi.conflicting) return DartCallLayoutStatus::kConflictingEvidence;
    if (!IsProven(abi.result) ||
        std::any_of(abi.parameters.begin(), abi.parameters.end(),
                    [](const auto& slot) { return !IsProven(slot); }) ||
        !abi.has_stack_calling_convention || !abi.has_optional_parameter_info ||
        !abi.has_max_parameters_in_registers) {
        return DartCallLayoutStatus::kIncompleteEvidence;
    }
    if (abi.has_optional_parameters && !closure_call) {
        return DartCallLayoutStatus::kOptionalArgumentsUnsupported;
    }
    if (!IsSupportedResultRepresentation(abi.result.representation) ||
        std::any_of(abi.parameters.begin(), abi.parameters.end(), [](const auto& slot) {
            return !IsSupportedParameterRepresentation(slot.representation);
        })) {
        return DartCallLayoutStatus::kUnsupportedRepresentation;
    }

    const uint32_t register_limit =
        abi.must_use_stack_calling_convention ? 0 : abi.max_parameters_in_registers;
    uint32_t next_gp = 0;
    uint32_t next_fpu = 0;
    std::vector<bool> needs_stack(abi.parameters.size(), false);
    DartCallLayout layout;
    layout.parameters.resize(abi.parameters.size());
    layout.dart_sp_register = profile.dart_sp_register;

    for (uint32_t index = 0; index < abi.parameters.size(); ++index) {
        const auto representation = abi.parameters[index].representation;
        auto& parameter = layout.parameters[index];
        parameter.representation = representation;

        if (index < register_limit) {
            if ((representation == DartAbiRepresentation::kTagged ||
                 representation == DartAbiRepresentation::kUnboxedInt64) &&
                next_gp < profile.gp_argument_register_count) {
                parameter.location.locations[0] = Gp(profile.gp_argument_registers[next_gp++]);
                parameter.location.count = 1;
            } else if (representation == DartAbiRepresentation::kUnboxedDouble &&
                       next_fpu < profile.fpu_argument_register_count) {
                parameter.location.locations[0] = Fpu(profile.fpu_argument_registers[next_fpu++]);
                parameter.location.count = 1;
            }
        }

        if (parameter.location.count == 0) {
            needs_stack[index] = true;
            ++layout.stack_words;
        }
    }

    // ComputeCallingConvention assigns the remaining stack parameters in
    // reverse formal order. The first assigned FP slot is exactly the
    // fp_to_entry_sp_slot_delta, making the last stack parameter entry-SP+0.
    int32_t fp_slot = profile.fp_to_entry_sp_slot_delta;
    for (size_t reverse = abi.parameters.size(); reverse > 0; --reverse) {
        const size_t index = reverse - 1;
        if (!needs_stack[index]) continue;
        const int32_t entry_sp_slot = fp_slot - profile.fp_to_entry_sp_slot_delta;
        layout.parameters[index].location.locations[0] =
            EntryStack(entry_sp_slot, profile.word_size);
        layout.parameters[index].location.count = 1;
        ++fp_slot;
    }

    layout.result.representation = abi.result.representation;
    switch (abi.result.representation) {
    case DartAbiRepresentation::kTagged:
        layout.result.location.locations[0] = Gp(profile.tagged_result_gp_register);
        layout.result.location.count = 1;
        break;
    case DartAbiRepresentation::kUnboxedInt64:
        layout.result.location.locations[0] = Gp(profile.unboxed_int64_result_gp_register);
        layout.result.location.count = 1;
        break;
    case DartAbiRepresentation::kUnboxedDouble:
        layout.result.location.locations[0] = Fpu(profile.unboxed_double_result_fpu_register);
        layout.result.location.count = 1;
        break;
    case DartAbiRepresentation::kPairOfTagged:
        layout.result.location.locations[0] = Gp(profile.pair_of_tagged_result_gp_registers[0]);
        layout.result.location.locations[1] = Gp(profile.pair_of_tagged_result_gp_registers[1]);
        layout.result.location.count = 2;
        break;
    case DartAbiRepresentation::kUnknown:
        return DartCallLayoutStatus::kIncompleteEvidence;
    }

    *out_layout = std::move(layout);
    return DartCallLayoutStatus::kOk;
}

bool ComputeArm64AotCallLayout(const DartFunctionAbiResolution& abi, DartCallLayout* out_layout) {
    return ComputeDartCallLayout(abi, Arm64AotCallingConventionProfile(), out_layout) ==
           DartCallLayoutStatus::kOk;
}

}  // namespace dartplant::abi
