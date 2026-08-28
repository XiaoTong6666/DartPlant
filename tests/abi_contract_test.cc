// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <array>

#include "abi/evidence_solver.h"
#include "test_runner.h"

namespace {

using dartplant::abi::DartAbiEvidenceSource;
using dartplant::abi::DartAbiProofState;
using dartplant::abi::DartAbiRepresentation;
using dartplant::abi::DartAbiSlotEvidence;
using dartplant::abi::DartFunctionAbiEvidence;

DartAbiSlotEvidence Proven(DartAbiRepresentation representation, DartAbiEvidenceSource source) {
    return {
        .representation = representation,
        .source = source,
        .proof = DartAbiProofState::kProven,
    };
}

}  // namespace

TEST_CASE(AbiContractCombinesMatchingIndependentEvidence) {
    const std::array evidence = {
        Proven(DartAbiRepresentation::kUnboxedDouble, DartAbiEvidenceSource::kCompilerOracle),
        Proven(DartAbiRepresentation::kUnboxedDouble, DartAbiEvidenceSource::kAotCodeAnalysis),
    };
    const auto result = dartplant::abi::ResolveSlotEvidence(evidence);
    EXPECT_EQ(static_cast<uint8_t>(DartAbiProofState::kProven), static_cast<uint8_t>(result.proof));
    EXPECT_EQ(static_cast<uint8_t>(DartAbiRepresentation::kUnboxedDouble),
              static_cast<uint8_t>(result.representation));
    EXPECT_TRUE(result.source_mask != 0);
}

TEST_CASE(AbiContractRejectsConflictingProvenRepresentations) {
    const std::array evidence = {
        Proven(DartAbiRepresentation::kTagged, DartAbiEvidenceSource::kCompilerOracle),
        Proven(DartAbiRepresentation::kUnboxedInt64, DartAbiEvidenceSource::kAotCodeAnalysis),
    };
    const auto result = dartplant::abi::ResolveSlotEvidence(evidence);
    EXPECT_EQ(static_cast<uint8_t>(DartAbiProofState::kConflicting),
              static_cast<uint8_t>(result.proof));
    EXPECT_EQ(static_cast<uint8_t>(DartAbiRepresentation::kUnknown),
              static_cast<uint8_t>(result.representation));
}

TEST_CASE(AbiContractFunctionResolutionFailsClosedOnMissingSlots) {
    DartFunctionAbiEvidence oracle;
    oracle.parameters = {
        Proven(DartAbiRepresentation::kUnboxedInt64, DartAbiEvidenceSource::kCompilerOracle),
    };
    oracle.result = Proven(DartAbiRepresentation::kTagged, DartAbiEvidenceSource::kCompilerOracle);

    const std::array providers = {oracle};
    const auto result = dartplant::abi::ResolveFunctionAbiEvidence(providers, 2);
    EXPECT_FALSE(result.fully_proven);
    EXPECT_FALSE(result.conflicting);
    EXPECT_EQ(static_cast<uint8_t>(DartAbiProofState::kUnknown),
              static_cast<uint8_t>(result.parameters[1].proof));
}
