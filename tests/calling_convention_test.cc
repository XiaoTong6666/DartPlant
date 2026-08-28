// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "abi/calling_convention.h"

#include "test_runner.h"

namespace {

using dartplant::abi::DartAbiProofState;
using dartplant::abi::DartAbiRepresentation;
using dartplant::abi::DartAbiResolvedSlot;
using dartplant::abi::DartCallLayout;
using dartplant::abi::DartCallLayoutStatus;
using dartplant::abi::DartFunctionAbiResolution;

DartAbiResolvedSlot Proven(DartAbiRepresentation representation) {
    return {
        .representation = representation,
        .proof = DartAbiProofState::kProven,
    };
}

DartFunctionAbiResolution FunctionAbi(std::initializer_list<DartAbiRepresentation> parameters,
                                      DartAbiRepresentation result, uint32_t max_registers,
                                      bool force_stack = false) {
    DartFunctionAbiResolution abi;
    for (const auto representation : parameters) abi.parameters.push_back(Proven(representation));
    abi.result = Proven(result);
    abi.has_stack_calling_convention = true;
    abi.must_use_stack_calling_convention = force_stack;
    abi.has_optional_parameter_info = true;
    abi.has_optional_parameters = false;
    abi.has_max_parameters_in_registers = true;
    abi.max_parameters_in_registers = max_registers;
    abi.fully_proven = true;
    return abi;
}

}  // namespace

TEST_CASE(CallingConventionProfileMatchesDartArm64SdkConstants) {
    const auto profile = dartplant::abi::Arm64AotCallingConventionProfile();
    EXPECT_EQ(8u, profile.word_size);
    EXPECT_EQ(15u, profile.dart_sp_register);
    EXPECT_EQ(6u, profile.gp_argument_register_count);
    EXPECT_EQ(6u, profile.fpu_argument_register_count);
    EXPECT_EQ(1u, profile.gp_argument_registers[0]);
    EXPECT_EQ(7u, profile.gp_argument_registers[5]);
    EXPECT_EQ(0u, profile.fpu_argument_registers[0]);
    EXPECT_EQ(5u, profile.fpu_argument_registers[5]);
    EXPECT_EQ(2, profile.fp_to_entry_sp_slot_delta);
}

TEST_CASE(CallingConventionSeparatesCpuAndFpuAllocators) {
    const auto abi =
        FunctionAbi({DartAbiRepresentation::kTagged, DartAbiRepresentation::kUnboxedDouble,
                     DartAbiRepresentation::kUnboxedInt64, DartAbiRepresentation::kUnboxedDouble},
                    DartAbiRepresentation::kUnboxedDouble, 4);
    DartCallLayout layout;
    EXPECT_EQ(static_cast<uint8_t>(DartCallLayoutStatus::kOk),
              static_cast<uint8_t>(dartplant::abi::ComputeDartCallLayout(
                  abi, dartplant::abi::Arm64AotCallingConventionProfile(), &layout)));
    EXPECT_EQ(1u, layout.parameters[0].location.locations[0].register_index);
    EXPECT_EQ(0u, layout.parameters[1].location.locations[0].register_index);
    EXPECT_EQ(2u, layout.parameters[2].location.locations[0].register_index);
    EXPECT_EQ(1u, layout.parameters[3].location.locations[0].register_index);
    EXPECT_EQ(static_cast<uint8_t>(dartplant::abi::DartAbiLocationKind::kFpuRegister),
              static_cast<uint8_t>(layout.result.location.locations[0].kind));
    EXPECT_EQ(0u, layout.stack_words);
}

TEST_CASE(CallingConventionUsesSdkEntrySpRelativeStackOrder) {
    const auto abi = FunctionAbi({DartAbiRepresentation::kTagged, DartAbiRepresentation::kTagged,
                                  DartAbiRepresentation::kTagged, DartAbiRepresentation::kTagged},
                                 DartAbiRepresentation::kTagged, 2);
    DartCallLayout layout;
    EXPECT_TRUE(dartplant::abi::ComputeArm64AotCallLayout(abi, &layout));
    EXPECT_EQ(static_cast<uint8_t>(dartplant::abi::DartAbiLocationKind::kEntryStack),
              static_cast<uint8_t>(layout.parameters[2].location.locations[0].kind));
    EXPECT_EQ(8, layout.parameters[2].location.locations[0].stack_offset);
    EXPECT_EQ(0, layout.parameters[3].location.locations[0].stack_offset);
    EXPECT_EQ(2u, layout.stack_words);
}

TEST_CASE(CallingConventionForcedStackPlacesLastFormalAtEntrySp) {
    const auto abi =
        FunctionAbi({DartAbiRepresentation::kTagged, DartAbiRepresentation::kUnboxedDouble},
                    DartAbiRepresentation::kTagged, 2, true);
    DartCallLayout layout;
    EXPECT_TRUE(dartplant::abi::ComputeArm64AotCallLayout(abi, &layout));
    EXPECT_EQ(8, layout.parameters[0].location.locations[0].stack_offset);
    EXPECT_EQ(0, layout.parameters[1].location.locations[0].stack_offset);
}

TEST_CASE(CallingConventionSupportsPairOfTaggedReturn) {
    const auto abi = FunctionAbi({}, DartAbiRepresentation::kPairOfTagged, 0);
    DartCallLayout layout;
    EXPECT_TRUE(dartplant::abi::ComputeArm64AotCallLayout(abi, &layout));
    EXPECT_EQ(2u, layout.result.location.count);
    EXPECT_EQ(0u, layout.result.location.locations[0].register_index);
    EXPECT_EQ(1u, layout.result.location.locations[1].register_index);
}

TEST_CASE(CallingConventionFailsClosedWithoutExactFunctionFlags) {
    auto abi = FunctionAbi({DartAbiRepresentation::kTagged}, DartAbiRepresentation::kTagged, 1);
    DartCallLayout layout;

    abi.has_stack_calling_convention = false;
    EXPECT_EQ(static_cast<uint8_t>(DartCallLayoutStatus::kIncompleteEvidence),
              static_cast<uint8_t>(dartplant::abi::ComputeDartCallLayout(
                  abi, dartplant::abi::Arm64AotCallingConventionProfile(), &layout)));

    abi.has_stack_calling_convention = true;
    abi.has_optional_parameter_info = false;
    EXPECT_FALSE(dartplant::abi::ComputeArm64AotCallLayout(abi, &layout));

    abi.has_optional_parameter_info = true;
    abi.has_max_parameters_in_registers = false;
    EXPECT_FALSE(dartplant::abi::ComputeArm64AotCallLayout(abi, &layout));
}

TEST_CASE(CallingConventionRejectsOptionalTransportUntilModeled) {
    auto abi = FunctionAbi({DartAbiRepresentation::kTagged}, DartAbiRepresentation::kTagged, 1);
    abi.has_optional_parameters = true;
    DartCallLayout layout;
    EXPECT_EQ(static_cast<uint8_t>(DartCallLayoutStatus::kOptionalArgumentsUnsupported),
              static_cast<uint8_t>(dartplant::abi::ComputeDartCallLayout(
                  abi, dartplant::abi::Arm64AotCallingConventionProfile(), &layout)));
}
