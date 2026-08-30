// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>

#include "abi/evidence_solver.h"
#include "dartplant/advanced/live_vm.h"
#include "test_runner.h"

static_assert(offsetof(DartPlantLiveVmProfile, code_entry_point_offset) == 88);
static_assert(offsetof(DartPlantLiveVmProfile, code_object_pool_offset) == 92);
static_assert(offsetof(DartPlantLiveVmProfile, function_entry_point_offset) == 104);
static_assert(offsetof(DartPlantLiveVmProfile, function_name_offset) == 108);
static_assert(offsetof(DartPlantLiveVmProfile, cid_two_byte_string) == 212);
static_assert(offsetof(DartPlantLiveVmProfile, code_unchecked_entry_point_offset) == 216);

static_assert(offsetof(DartPlantLiveVmFunctionInfo, function) == 8);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, code) == 16);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, code_object_pool) == 24);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, function_entry_point) == 32);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, code_entry_point) == 40);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, entry_va) == 48);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, code_section_va) == 56);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, code_size) == 64);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, function_kind) == 68);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, owner_class) == 72);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, library) == 80);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, owner_is_toplevel_class) == 88);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, function_name) == 96);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, class_name) == 256);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, library_uri) == 416);
static_assert(offsetof(DartPlantLiveVmFunctionInfo, entry_alias_counts) == 736);

static_assert(offsetof(DartPlantLiveVmProbeInfo, reserved_count) == 20);
static_assert(offsetof(DartPlantLiveVmProbeInfo, thread) == 24);
static_assert(offsetof(DartPlantLiveVmProbeInfo, code) == 112);
static_assert(offsetof(DartPlantLiveVmProbeInfo, function) == 136);
static_assert(offsetof(DartPlantLiveVmProbeInfo, owner_class) == 152);
static_assert(offsetof(DartPlantLiveVmProbeInfo, heap_bits_match) == 168);
static_assert(offsetof(DartPlantLiveVmProbeInfo, function_name) == 185);
static_assert(offsetof(DartPlantLiveVmProbeInfo, class_name) == 345);
static_assert(offsetof(DartPlantLiveVmProbeInfo, library_uri) == 505);
static_assert(offsetof(DartPlantLiveVmProbeInfo, requested_entry_kind) == 832);

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
