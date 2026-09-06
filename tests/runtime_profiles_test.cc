// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "vm/runtime_profiles.h"

#include <array>
#include <string_view>

#include "test_runner.h"

TEST_CASE(RuntimeProfilesMatchDartArm64CallingConvention) {
    constexpr std::array<uint8_t, 6> kExpectedGp = {1, 2, 3, 5, 6, 7};
    constexpr std::array<uint8_t, 6> kExpectedFpu = {0, 1, 2, 3, 4, 5};

    EXPECT_EQ(3U, dartplant::RuntimeProfileCount());
    for (size_t index = 0; index < dartplant::RuntimeProfileCount(); ++index) {
        const auto& profile = dartplant::RuntimeProfiles()[index];
        EXPECT_EQ(15U, profile.dart_sp_register);
        EXPECT_EQ(4U, profile.arguments_descriptor_register);
        EXPECT_TRUE(profile.dart_gp_argument_registers == kExpectedGp);
        EXPECT_TRUE(profile.dart_fpu_argument_registers == kExpectedFpu);
        EXPECT_EQ(8U, profile.instructions_monomorphic_entry_offset_aot);
        EXPECT_EQ(24U, profile.instructions_polymorphic_entry_offset_aot);
        EXPECT_EQ(0x8U, profile.live_vm.code_entry_point_offset);
        EXPECT_EQ(0x18U, profile.live_vm.code_unchecked_entry_point_offset);
        EXPECT_EQ(0x10U, profile.live_vm.code_monomorphic_entry_point_offset);
        EXPECT_EQ(0x20U, profile.live_vm.code_monomorphic_unchecked_entry_point_offset);
        EXPECT_EQ(0x8U, profile.live_vm.function_entry_point_offset);
        EXPECT_EQ(0x10U, profile.live_vm.function_unchecked_entry_point_offset);
        EXPECT_TRUE(profile.abi_id != nullptr && profile.abi_id[0] != '\0');
        EXPECT_EQ(8U, profile.machine.pointer_size);
        EXPECT_TRUE(profile.machine.product);
        EXPECT_TRUE(profile.machine.compressed_pointers);
        EXPECT_TRUE(profile.thread_bridge.enter_safepoint_stub_offset != 0);
        EXPECT_TRUE(profile.thread_bridge.exit_safepoint_stub_offset != 0);
        EXPECT_TRUE(profile.thread_bridge.execution_state_offset != 0);
        EXPECT_TRUE(profile.thread_bridge.exit_through_ffi_offset != 0);
        EXPECT_TRUE(profile.type_arguments.cid != 0);
        EXPECT_EQ(0xCU, profile.type_arguments.length_offset);
        EXPECT_EQ(0x18U, profile.type_arguments.types_offset);
        EXPECT_EQ(8U, profile.transition.vm_tag_dart);
        EXPECT_EQ(1U, profile.transition.execution_generated);
        EXPECT_EQ(2U, profile.transition.execution_native);
        EXPECT_EQ(1U, profile.transition.exit_through_ffi);
        EXPECT_EQ(2U, profile.transition.exit_through_runtime_call);
        EXPECT_EQ(profile.live_vm.profile_version,
                  dartplant::FindRuntimeProfileByVersion(profile.live_vm.profile_version)
                      ->live_vm.profile_version);
        EXPECT_TRUE(profile.thread_jump_to_frame_entry_point_offset != 0);
    }
}

TEST_CASE(RuntimeProfileCandidatesUseSnapshotAsHintNotArtifactGate) {
    dartplant::VmRuntimeFacts facts{};
    facts.snapshot_hash = "d20a1be77c3d3c41b2a5accaee1ce549";
    facts.snapshot_features = "arm64 product compressed-pointers";
    auto candidates = dartplant::ResolveRuntimeProfileCandidates(facts);
    EXPECT_EQ(dartplant::RuntimeProfileCount(), candidates.size());
    EXPECT_EQ(1U, candidates[0]->live_vm.profile_version);

    facts.snapshot_hash = "ffffffffffffffffffffffffffffffff";
    candidates = dartplant::ResolveRuntimeProfileCandidates(facts);
    EXPECT_EQ(dartplant::RuntimeProfileCount(), candidates.size());

    facts.snapshot_features = "arm64 product";
    candidates = dartplant::ResolveRuntimeProfileCandidates(facts);
    EXPECT_EQ(0U, candidates.size());
}

TEST_CASE(RuntimeProfileAbiIdentityTracksPrivateLayoutNotArtifactIdentity) {
    const auto* profiles = dartplant::RuntimeProfiles();
    EXPECT_TRUE(std::string_view(profiles[0].abi_id) != std::string_view(profiles[1].abi_id));
    EXPECT_TRUE(std::string_view(profiles[0].abi_id) != std::string_view(profiles[2].abi_id));
    EXPECT_TRUE(std::string_view(profiles[1].abi_id) != std::string_view(profiles[2].abi_id));

    const auto* dart344 = dartplant::FindRuntimeProfileByVersion(1);
    EXPECT_TRUE(dart344 != nullptr);
    EXPECT_EQ(0x1D0U, dart344->thread_bridge.enter_safepoint_stub_offset);
    EXPECT_EQ(0x1D8U, dart344->thread_bridge.exit_safepoint_stub_offset);
    EXPECT_EQ(0x710U, dart344->thread_bridge.top_exit_frame_offset);
    EXPECT_EQ(0x730U, dart344->thread_bridge.vm_tag_offset);
    EXPECT_EQ(0x748U, dart344->thread_bridge.active_exception_offset);
    EXPECT_EQ(0x750U, dart344->thread_bridge.active_stacktrace_offset);
    EXPECT_EQ(0x770U, dart344->thread_bridge.execution_state_offset);
    EXPECT_EQ(0x780U, dart344->thread_bridge.exit_through_ffi_offset);
    EXPECT_EQ(46U, dart344->type_arguments.cid);
}

TEST_CASE(RuntimeProfilesReconstructPrecompiledCodePayloadStartExactly) {
    constexpr uint64_t kPayload = 0x100000;
    constexpr uint32_t kLength = 0x80;
    dartplant::AotCodePayloadRange range{};
    EXPECT_TRUE(
        dartplant::ComputeAotCodePayloadRange(1, kPayload + 24, kPayload + 8, kLength, &range));
    EXPECT_EQ(kPayload, range.start);
    EXPECT_EQ(kPayload + kLength, range.end);
    EXPECT_TRUE(range.has_monomorphic_entry);
    EXPECT_EQ(kLength - 24U, range.end - (kPayload + 24));
    EXPECT_EQ(kLength - 8U, range.end - (kPayload + 8));

    EXPECT_TRUE(dartplant::ComputeAotCodePayloadRange(1, kPayload, kPayload, kLength, &range));
    EXPECT_EQ(kPayload, range.start);
    EXPECT_EQ(kPayload + kLength, range.end);
    EXPECT_FALSE(range.has_monomorphic_entry);

    EXPECT_FALSE(
        dartplant::ComputeAotCodePayloadRange(1, kPayload + 24, kPayload + 12, kLength, &range));
}

TEST_CASE(RuntimeProfilesBindSnapshotToAllPrivateLayouts) {
    const auto* dart344 = dartplant::FindRuntimeProfileBySnapshot(
        "d20a1be77c3d3c41b2a5accaee1ce549", "flutter-arm64-product-compressed");
    EXPECT_TRUE(dart344 != nullptr);
    EXPECT_EQ(0x268U, dart344->thread_jump_to_frame_entry_point_offset);
    EXPECT_EQ(0x78U, dart344->canonical_bool.thread_true_offset);
    EXPECT_EQ(0x20U, dart344->function_type.function_signature_offset);
    EXPECT_EQ(0x20U, dart344->function_type.type_parameters_offset);

    const auto* dart3121 = dartplant::FindRuntimeProfileBySnapshot(
        "ace654289f5abc240509fc941453ebc5", "flutter-arm64-product-compressed");
    EXPECT_TRUE(dart3121 != nullptr);
    EXPECT_EQ(0x278U, dart3121->thread_jump_to_frame_entry_point_offset);
    EXPECT_EQ(23U, dart3121->live_vm.cid_object_pool);
    EXPECT_EQ(63U, dart3121->canonical_bool.cid);
    EXPECT_EQ(50U, dart3121->function_type.cid_function_type);

    EXPECT_TRUE(dartplant::FindRuntimeProfileBySnapshot("unknown") == nullptr);
    EXPECT_EQ(0U, dartplant::ThreadJumpToFrameOffsetForSnapshot("unknown"));
}
