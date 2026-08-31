// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_SRC_ABI_EVIDENCE_H_
#define DARTPLANT_SRC_ABI_EVIDENCE_H_

#include <stdint.h>

#include <vector>

#include "abi/call_layout.h"
#include "abi/representation.h"

namespace dartplant::abi {

enum class DartAbiEvidenceSource : uint8_t {
    kUnknown = 0,
    kCompilerOracle,
    kAotCodeAnalysis,
    kVerifiedFixture,
};

enum class DartAbiProofState : uint8_t {
    kUnknown = 0,
    kProven,
    kConflicting,
};

// Machine-code evidence is keyed by a physical entry location. It does not
// fabricate a logical parameter index; that join belongs to the later solver.
struct DartAbiObservation {
    DartAbiLocation location{};
    DartAbiRepresentation representation = DartAbiRepresentation::kUnknown;
    DartAbiEvidenceSource source = DartAbiEvidenceSource::kUnknown;
    DartAbiProofState proof = DartAbiProofState::kUnknown;
    uint64_t first_observation_pc = 0;
    uint32_t observation_count = 0;
};

struct DartAbiSlotEvidence {
    DartAbiRepresentation representation = DartAbiRepresentation::kUnknown;
    DartAbiEvidenceSource source = DartAbiEvidenceSource::kUnknown;
    DartAbiProofState proof = DartAbiProofState::kUnknown;
};

struct DartAbiResolvedSlot {
    DartAbiRepresentation representation = DartAbiRepresentation::kUnknown;
    DartAbiProofState proof = DartAbiProofState::kUnknown;
    uint32_t source_mask = 0;
};

struct DartFunctionAbiEvidence {
    std::vector<DartAbiSlotEvidence> parameters;
    DartAbiSlotEvidence result{};

    bool has_stack_calling_convention = false;
    bool must_use_stack_calling_convention = false;
    bool has_overrides_with_less_direct_parameters = false;
    bool has_optional_parameter_info = false;
    bool has_optional_parameters = false;
    bool has_max_parameters_in_registers = false;
    uint32_t max_parameters_in_registers = 0;
    bool is_closure = false;
};

struct DartFunctionAbiResolution {
    std::vector<DartAbiResolvedSlot> parameters;
    DartAbiResolvedSlot result{};

    bool has_stack_calling_convention = false;
    bool must_use_stack_calling_convention = false;
    bool has_overrides_with_less_direct_parameters = false;
    bool has_optional_parameter_info = false;
    bool has_optional_parameters = false;
    bool has_max_parameters_in_registers = false;
    uint32_t max_parameters_in_registers = 0;
    bool is_closure = false;

    bool fully_proven = false;
    bool conflicting = false;
};

constexpr uint32_t EvidenceSourceMask(DartAbiEvidenceSource source) {
    const auto value = static_cast<uint32_t>(source);
    return value == 0 ? 0u : (1u << (value - 1u));
}

}  // namespace dartplant::abi

#endif  // DARTPLANT_SRC_ABI_EVIDENCE_H_
