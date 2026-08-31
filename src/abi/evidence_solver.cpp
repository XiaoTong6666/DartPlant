// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "abi/evidence_solver.h"

#include <algorithm>
#include <vector>

namespace dartplant::abi {

DartAbiResolvedSlot ResolveSlotEvidence(std::span<const DartAbiSlotEvidence> evidence) {
    DartAbiResolvedSlot resolved;
    for (const auto& item : evidence) {
        if (item.proof == DartAbiProofState::kUnknown ||
            item.representation == DartAbiRepresentation::kUnknown) {
            continue;
        }
        resolved.source_mask |= EvidenceSourceMask(item.source);
        if (item.proof == DartAbiProofState::kConflicting) {
            resolved.representation = DartAbiRepresentation::kUnknown;
            resolved.proof = DartAbiProofState::kConflicting;
            continue;
        }
        if (resolved.proof == DartAbiProofState::kUnknown) {
            resolved.representation = item.representation;
            resolved.proof = DartAbiProofState::kProven;
            continue;
        }
        if (resolved.proof == DartAbiProofState::kProven &&
            resolved.representation != item.representation) {
            resolved.representation = DartAbiRepresentation::kUnknown;
            resolved.proof = DartAbiProofState::kConflicting;
        }
    }
    return resolved;
}

DartFunctionAbiResolution ResolveFunctionAbiEvidence(
    std::span<const DartFunctionAbiEvidence> providers, uint32_t parameter_count) {
    DartFunctionAbiResolution result;
    result.parameters.resize(parameter_count);

    for (uint32_t index = 0; index < parameter_count; ++index) {
        std::vector<DartAbiSlotEvidence> slot_evidence;
        slot_evidence.reserve(providers.size());
        for (const auto& provider : providers) {
            if (index < provider.parameters.size()) {
                slot_evidence.push_back(provider.parameters[index]);
            }
        }
        result.parameters[index] = ResolveSlotEvidence(slot_evidence);
    }

    std::vector<DartAbiSlotEvidence> result_evidence;
    result_evidence.reserve(providers.size());
    for (const auto& provider : providers) {
        result_evidence.push_back(provider.result);
    }
    result.result = ResolveSlotEvidence(result_evidence);

    bool stack_cc_seen = false;
    bool stack_cc_value = false;
    bool optional_seen = false;
    bool optional_value = false;
    bool max_regs_seen = false;
    uint32_t max_regs_value = 0;
    bool closure_seen = false;
    bool closure_value = false;
    for (const auto& provider : providers) {
        result.has_overrides_with_less_direct_parameters |=
            provider.has_overrides_with_less_direct_parameters;

        if (!closure_seen) {
            closure_seen = true;
            closure_value = provider.is_closure;
        } else if (closure_value != provider.is_closure) {
            result.conflicting = true;
        }

        if (provider.has_optional_parameter_info) {
            if (!optional_seen) {
                optional_seen = true;
                optional_value = provider.has_optional_parameters;
            } else if (optional_value != provider.has_optional_parameters) {
                result.conflicting = true;
            }
        }

        if (provider.has_stack_calling_convention) {
            if (!stack_cc_seen) {
                stack_cc_seen = true;
                stack_cc_value = provider.must_use_stack_calling_convention;
            } else if (stack_cc_value != provider.must_use_stack_calling_convention) {
                result.conflicting = true;
            }
        }

        if (provider.has_max_parameters_in_registers) {
            if (!max_regs_seen) {
                max_regs_seen = true;
                max_regs_value = provider.max_parameters_in_registers;
            } else if (max_regs_value != provider.max_parameters_in_registers) {
                result.conflicting = true;
            }
        }
    }
    result.has_stack_calling_convention = stack_cc_seen && !result.conflicting;
    result.must_use_stack_calling_convention = stack_cc_value;
    result.has_optional_parameter_info = optional_seen && !result.conflicting;
    result.has_optional_parameters = optional_value;
    result.has_max_parameters_in_registers = max_regs_seen && !result.conflicting;
    result.max_parameters_in_registers = max_regs_value;
    result.is_closure = closure_value;

    const auto slot_conflicting = [](const DartAbiResolvedSlot& slot) {
        return slot.proof == DartAbiProofState::kConflicting;
    };
    result.conflicting |=
        std::any_of(result.parameters.begin(), result.parameters.end(), slot_conflicting) ||
        slot_conflicting(result.result);

    const auto slot_proven = [](const DartAbiResolvedSlot& slot) {
        return slot.proof == DartAbiProofState::kProven &&
               slot.representation != DartAbiRepresentation::kUnknown;
    };
    result.fully_proven =
        !result.conflicting && result.has_stack_calling_convention &&
        result.has_optional_parameter_info && result.has_max_parameters_in_registers &&
        slot_proven(result.result) &&
        std::all_of(result.parameters.begin(), result.parameters.end(), slot_proven);
    return result;
}

}  // namespace dartplant::abi
