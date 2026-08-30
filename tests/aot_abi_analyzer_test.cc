// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "aot_abi_analyzer.h"

#include "test_runner.h"

TEST_CASE(AotAbiAnalyzerProvesDirectDoubleArguments) {
    // fadd d0, d0, d1; ret
    const uint8_t code[] = {0x00, 0x28, 0x61, 0x1e, 0xc0, 0x03, 0x5f, 0xd6};
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnboxedDouble),
              static_cast<uint8_t>(result.fpu_arguments[0]));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnboxedDouble),
              static_cast<uint8_t>(result.fpu_arguments[1]));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnknown),
              static_cast<uint8_t>(result.fpu_arguments[2]));
}

TEST_CASE(AotAbiAnalyzerTracksForwardedDoubleArguments) {
    // fmov d2, d0; fadd d3, d2, d1; ret
    const uint8_t code[] = {
        0x02, 0x40, 0x60, 0x1e, 0x43, 0x28, 0x61, 0x1e, 0xc0, 0x03, 0x5f, 0xd6,
    };
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnboxedDouble),
              static_cast<uint8_t>(result.fpu_arguments[0]));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnboxedDouble),
              static_cast<uint8_t>(result.fpu_arguments[1]));
}

TEST_CASE(AotAbiAnalyzerDoesNotTreatDoubleDestinationAsIncomingArgument) {
    // fmov d3, #1.5; fmul d4, d0, d3; ret. D4 is an allocator temporary, not
    // an incoming argument, even though entry-state tracking initially assigns
    // provenance to all six potential Dart FPU argument registers.
    const uint8_t code[] = {
        0x03, 0x10, 0x6f, 0x1e, 0x04, 0x08, 0x63, 0x1e, 0xc0, 0x03, 0x5f, 0xd6,
    };
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnboxedDouble),
              static_cast<uint8_t>(result.fpu_arguments[0]));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnknown),
              static_cast<uint8_t>(result.fpu_arguments[4]));
}

TEST_CASE(AotAbiAnalyzerLeavesUnusedArgumentsUnknown) {
    const uint8_t code[] = {0xc0, 0x03, 0x5f, 0xd6};  // ret
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnknown),
              static_cast<uint8_t>(result.fpu_arguments[0]));
    EXPECT_FALSE(result.stopped_at_control_flow);
}

TEST_CASE(AotAbiAnalyzerUsesDartX15AsEntryStackBase) {
    // ldr d0, [x15, #16]; fadd d1, d0, d0; ret
    const uint8_t code[] = {
        0xe0, 0x09, 0x40, 0xfd, 0x01, 0x28, 0x60, 0x1e, 0xc0, 0x03, 0x5f, 0xd6,
    };
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code));
    const dartplant::aot::AbiLocation stack_location = {
        .kind = dartplant::aot::LocationKind::kEntryStack,
        .stack_offset = 16,
    };
    const auto* observation = dartplant::aot::FindObservation(result, stack_location);
    EXPECT_TRUE(observation != nullptr);
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnboxedDouble),
              static_cast<uint8_t>(observation->representation));
}

TEST_CASE(AotAbiAnalyzerDoesNotTreatArchitectureSpAsDartEntryStack) {
    // ldr d0, [sp, #16]; fadd d1, d0, d0; ret
    const uint8_t code[] = {
        0xe0, 0x0b, 0x40, 0xfd, 0x01, 0x28, 0x60, 0x1e, 0xc0, 0x03, 0x5f, 0xd6,
    };
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code));
    const dartplant::aot::AbiLocation stack_location = {
        .kind = dartplant::aot::LocationKind::kEntryStack,
        .stack_offset = 16,
    };
    EXPECT_TRUE(dartplant::aot::FindObservation(result, stack_location) == nullptr);
}

TEST_CASE(AotAbiAnalyzerDropsVolatileProvenanceAcrossDirectCalls) {
    // fmov d2, d0; bl target; fadd d3, d2, d1; ret; target: ret
    const uint8_t code[] = {
        0x02, 0x40, 0x60, 0x1e, 0x03, 0x00, 0x00, 0x94, 0x43, 0x28,
        0x61, 0x1e, 0xc0, 0x03, 0x5f, 0xd6, 0xc0, 0x03, 0x5f, 0xd6,
    };
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnknown),
              static_cast<uint8_t>(result.fpu_arguments[0]));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnknown),
              static_cast<uint8_t>(result.fpu_arguments[1]));
}

TEST_CASE(AotAbiAnalyzerPublishesStructuralCallAndReturnEvidence) {
    // bl +8; ret; ret
    const uint8_t code[] = {
        0x02, 0x00, 0x00, 0x94, 0xc0, 0x03, 0x5f, 0xd6, 0xc0, 0x03, 0x5f, 0xd6,
    };
    constexpr uint64_t kAddress = 0x1000;
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code), kAddress);
    EXPECT_EQ(1U, result.direct_call_count);
    EXPECT_EQ(kAddress, result.direct_calls[0].site);
    EXPECT_EQ(kAddress + 8, result.direct_calls[0].target);
    EXPECT_EQ(1U, result.return_site_count);
    EXPECT_EQ(kAddress + 4, result.return_sites[0]);
    EXPECT_EQ(0U, result.external_branch_count);
    EXPECT_EQ(0U, result.indirect_call_count);
    EXPECT_EQ(0U, result.indirect_branch_count);
}

TEST_CASE(AotAbiAnalyzerPublishesOutOfFunctionBranchEdge) {
    // b +0x100; ret
    const uint8_t code[] = {0x40, 0x00, 0x00, 0x14, 0xc0, 0x03, 0x5f, 0xd6};
    constexpr uint64_t kAddress = 0x2000;
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code), kAddress);
    EXPECT_EQ(1U, result.external_branch_count);
    EXPECT_EQ(kAddress, result.external_branches[0].site);
    EXPECT_EQ(kAddress + 0x100, result.external_branches[0].target);
}

TEST_CASE(AotAbiAnalyzerTreatsIndirectCallAsReturningCall) {
    // blr x9; fadd d2, d0, d1; ret
    const uint8_t code[] = {
        0x20, 0x01, 0x3f, 0xd6, 0x02, 0x28, 0x61, 0x1e, 0xc0, 0x03, 0x5f, 0xd6,
    };
    constexpr uint64_t kAddress = 0x3000;
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code), kAddress);
    EXPECT_EQ(1U, result.indirect_call_count);
    EXPECT_EQ(kAddress, result.indirect_call_sites[0]);
    EXPECT_EQ(0U, result.indirect_branch_count);
    EXPECT_FALSE(result.has_unknown_control_flow);
    // BLR clobbers volatile FPU provenance, so the post-call FADD cannot prove
    // that d0/d1 are the original incoming arguments.
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnknown),
              static_cast<uint8_t>(result.fpu_arguments[0]));
}

TEST_CASE(AotAbiAnalyzerMatchesRegisterIndependentAdrpAddMaterialization) {
    // adrp x9, current-page; nop; add x10, x9, #0; ret. The semantic matcher
    // follows the producer/consumer register relationship rather than fixing a
    // particular destination register or requiring the ADD to be adjacent.
    const uint8_t code[] = {
        0x09, 0x00, 0x00, 0x90, 0x1f, 0x20, 0x03, 0xd5,
        0x2a, 0x01, 0x00, 0x91, 0xc0, 0x03, 0x5f, 0xd6,
    };
    constexpr uint64_t kAddress = 0x4000;
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code), kAddress);
    EXPECT_EQ(1U, result.address_materialization_count);
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::AddressMaterializationKind::kAdrpAdd),
              static_cast<uint8_t>(result.address_materializations[0].kind));
    EXPECT_EQ(10U, result.address_materializations[0].destination_register);
    EXPECT_EQ(kAddress, result.address_materializations[0].target);
    EXPECT_EQ(kAddress, result.address_materializations[0].site);
    EXPECT_EQ(kAddress + 8, result.address_materializations[0].completion_site);
}

TEST_CASE(AotAbiAnalyzerRejectsAdrpRelationAfterBaseRegisterClobber) {
    // adrp x11, current-page; mov x11, x0; add x12, x11, #0; ret. The ADD no
    // longer consumes the ADRP value, so no address relationship is proven.
    const uint8_t code[] = {
        0x0b, 0x00, 0x00, 0x90, 0xeb, 0x03, 0x00, 0xaa,
        0x6c, 0x01, 0x00, 0x91, 0xc0, 0x03, 0x5f, 0xd6,
    };
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code), 0x5000);
    EXPECT_EQ(0U, result.address_materialization_count);
}

TEST_CASE(AotStructuralRelationshipProofRequiresUniqueCandidate) {
    dartplant::aot::AnalysisResult first{};
    first.direct_calls[0] = {.site = 0x1010, .target = 0x9000};
    first.direct_call_count = 1;
    first.address_materializations[0] = {
        .kind = dartplant::aot::AddressMaterializationKind::kAdrpAdd,
        .destination_register = 9,
        .site = 0x1000,
        .completion_site = 0x1008,
        .target = 0xa000,
    };
    first.address_materialization_count = 1;
    first.return_sites[0] = 0x1020;
    first.return_site_count = 1;

    dartplant::aot::AnalysisResult second = first;
    second.address_materializations[0].target = 0xb000;

    const std::array candidates = {
        dartplant::aot::StructuralCandidate{.id = 1, .analysis = &first},
        dartplant::aot::StructuralCandidate{.id = 2, .analysis = &second},
    };
    const std::array constraints = {
        dartplant::aot::StructuralRelationConstraint{
            .kind = dartplant::aot::StructuralRelationKind::kDirectCallTarget,
            .value = 0x9000,
        },
        dartplant::aot::StructuralRelationConstraint{
            .kind = dartplant::aot::StructuralRelationKind::kAddressTarget,
            .value = 0xa000,
        },
        dartplant::aot::StructuralRelationConstraint{
            .kind = dartplant::aot::StructuralRelationKind::kReturnSite,
            .value = 0,
        },
    };
    const auto unique = dartplant::aot::SelectUniqueStructuralCandidate(candidates, constraints);
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::StructuralSelectionStatus::kUnique),
              static_cast<uint8_t>(unique.status));
    EXPECT_EQ(2U, unique.candidate_count);
    EXPECT_EQ(1U, unique.matching_count);
    EXPECT_EQ(1U, unique.selected_id);

    const std::array shared_only = {constraints[0], constraints[2]};
    const auto ambiguous = dartplant::aot::SelectUniqueStructuralCandidate(candidates, shared_only);
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::StructuralSelectionStatus::kAmbiguous),
              static_cast<uint8_t>(ambiguous.status));
    EXPECT_EQ(2U, ambiguous.matching_count);
    EXPECT_EQ(0U, ambiguous.selected_id);
}

TEST_CASE(AotAbiAnalyzerFailsClosedAtIndirectBranch) {
    // br x9; fadd d2, d0, d1; ret. Unlike BLR, BR has no known fallthrough and
    // therefore invalidates every ABI proof derived from the incomplete CFG.
    const uint8_t code[] = {
        0x20, 0x01, 0x1f, 0xd6, 0x02, 0x28, 0x61, 0x1e, 0xc0, 0x03, 0x5f, 0xd6,
    };
    constexpr uint64_t kAddress = 0x4000;
    const auto result = dartplant::aot::AnalyzeArm64Entry(code, sizeof(code), kAddress);
    EXPECT_EQ(0U, result.indirect_call_count);
    EXPECT_EQ(1U, result.indirect_branch_count);
    EXPECT_EQ(kAddress, result.indirect_branch_sites[0]);
    EXPECT_TRUE(result.has_unknown_control_flow);
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::ParameterEvidence::kUnknown),
              static_cast<uint8_t>(result.fpu_arguments[0]));
}

TEST_CASE(AotStructuralRelationshipProofFailsClosedWhenFactsAreTruncated) {
    std::vector<uint8_t> code;
    code.reserve((dartplant::aot::kMaxStructuralSites + 2) * 4);
    for (size_t index = 0; index < dartplant::aot::kMaxStructuralSites + 1; ++index) {
        code.insert(code.end(), {0x00, 0x00, 0x00, 0x94});  // bl .
    }
    code.insert(code.end(), {0xc0, 0x03, 0x5f, 0xd6});  // ret
    const auto result = dartplant::aot::AnalyzeArm64Entry(code.data(), code.size(), 0x8000);
    EXPECT_TRUE(result.structural_evidence_truncated);
    EXPECT_EQ(dartplant::aot::kMaxStructuralSites, result.direct_call_count);

    const std::array candidates = {
        dartplant::aot::StructuralCandidate{.id = 1, .analysis = &result},
    };
    const std::array constraints = {
        dartplant::aot::StructuralRelationConstraint{
            .kind = dartplant::aot::StructuralRelationKind::kDirectCallTarget,
            .value = 0x8000,
        },
    };
    const auto selection = dartplant::aot::SelectUniqueStructuralCandidate(candidates, constraints);
    EXPECT_EQ(static_cast<uint8_t>(dartplant::aot::StructuralSelectionStatus::kNoMatch),
              static_cast<uint8_t>(selection.status));
}
