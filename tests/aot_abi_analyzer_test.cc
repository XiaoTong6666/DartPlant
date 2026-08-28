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
