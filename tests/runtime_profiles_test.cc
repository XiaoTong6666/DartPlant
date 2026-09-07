// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "vm/runtime_profiles.h"

#include <array>
#include <cstring>
#include <string_view>

#include "test_runner.h"
#include "vm/abi/resolver.h"

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

TEST_CASE(VmArtifactLifecycleTreatsBuildIdAsIncarnationNotCompatibility) {
    dartplant::ModuleImage app{};
    app.name = "libapp.so";
    app.path = "/data/app/example/lib/arm64/libapp.so";
    app.build_id = "aaaa";
    app.load_bias = 0x100000;
    app.executable_ranges.push_back({
        .start = 0x101000,
        .end = 0x102000,
        .file_offset = 0x1000,
        .virtual_address = 0x1000,
        .file_size = 0x1000,
    });

    dartplant::ModuleImage engine{};
    engine.name = "libflutter.so";
    engine.path = "/data/app/example/lib/arm64/libflutter.so";
    engine.build_id = "bbbb";
    engine.load_bias = 0x200000;
    engine.executable_ranges.push_back({
        .start = 0x201000,
        .end = 0x202000,
        .file_offset = 0x2000,
        .virtual_address = 0x1000,
        .file_size = 0x1000,
    });

    dartplant::vm_abi::ArtifactSet expected{};
    expected.app = {
        .name = app.name,
        .path = app.path,
        .build_id = app.build_id,
        .load_bias = app.load_bias,
        .executable_ranges = app.executable_ranges,
    };
    expected.engines.push_back({
        .name = engine.name,
        .path = engine.path,
        .build_id = engine.build_id,
        .load_bias = engine.load_bias,
        .executable_ranges = engine.executable_ranges,
    });

    std::vector<dartplant::ModuleImage> current = {app, engine};
    EXPECT_TRUE(dartplant::vm_abi::ValidateArtifactSet(expected, current));

    // Build ids are intentionally not consulted by ABI candidate selection,
    // but once a binding is published they identify that concrete mapped
    // artifact incarnation and must change the lifecycle generation.
    current[1].build_id = "cccc";
    EXPECT_FALSE(dartplant::vm_abi::ValidateArtifactSet(expected, current));
    current[1] = engine;
    current[0].build_id.clear();
    EXPECT_FALSE(dartplant::vm_abi::ValidateArtifactSet(expected, current));
    current[0] = app;
    current[0].load_bias += 0x1000;
    EXPECT_FALSE(dartplant::vm_abi::ValidateArtifactSet(expected, current));

    current[0] = app;
    current[0].executable_ranges[0].end += 0x1000;
    EXPECT_FALSE(dartplant::vm_abi::ValidateArtifactSet(expected, current));

    current[0] = app;
    current[0].executable_ranges[0].file_offset += 0x1000;
    EXPECT_FALSE(dartplant::vm_abi::ValidateArtifactSet(expected, current));

    // Binding is scoped to the engine that supplied this adapter's API-DL
    // table. Loading another unrelated Flutter engine must not stale it.
    dartplant::ModuleImage other_engine = engine;
    other_engine.path = "/data/app/other/lib/arm64/libflutter.so";
    other_engine.build_id = "dddd";
    other_engine.load_bias = 0x300000;
    other_engine.executable_ranges[0].start = 0x301000;
    other_engine.executable_ranges[0].end = 0x302000;
    current = {app, engine, other_engine};
    EXPECT_TRUE(dartplant::vm_abi::ValidateArtifactSet(expected, current));

    // Conversely, two expected engine incarnations cannot both consume the
    // same current module while an unrelated engine merely satisfies the
    // process-wide module count.
    expected.engines.push_back(expected.engines.front());
    EXPECT_FALSE(dartplant::vm_abi::ValidateArtifactSet(expected, current));
}

TEST_CASE(VmEngineAnchorBindsExactlyOneFlutterIncarnation) {
    dartplant::ModuleImage first{};
    first.name = "libflutter.so";
    first.path = "/data/app/first/lib/arm64/libflutter.so";
    first.build_id = "1111";
    first.load_bias = 0x200000;
    first.executable_ranges.push_back({
        .start = 0x201000,
        .end = 0x202000,
        .file_offset = 0x1000,
        .virtual_address = 0x1000,
        .file_size = 0x1000,
    });

    dartplant::ModuleImage second{};
    second.name = "libflutter.so";
    second.path = "/data/app/second/lib/arm64/libflutter.so";
    second.build_id = "2222";
    second.load_bias = 0x300000;
    second.executable_ranges.push_back({
        .start = 0x301000,
        .end = 0x302000,
        .file_offset = 0x1000,
        .virtual_address = 0x1000,
        .file_size = 0x1000,
    });

    std::vector<dartplant::ModuleImage> modules = {first, second};
    dartplant::vm_abi::ArtifactIncarnation resolved{};
    EXPECT_TRUE(dartplant::vm_abi::ResolveEngineIncarnationForAnchor(modules, 0x201100, &resolved));
    EXPECT_EQ(first.path, resolved.path);
    EXPECT_EQ(first.build_id, resolved.build_id);
    EXPECT_EQ(first.load_bias, resolved.load_bias);
    EXPECT_EQ(first.executable_ranges.size(), resolved.executable_ranges.size());
    EXPECT_EQ(first.executable_ranges[0].start, resolved.executable_ranges[0].start);
    EXPECT_EQ(first.executable_ranges[0].end, resolved.executable_ranges[0].end);
    EXPECT_EQ(first.executable_ranges[0].file_offset, resolved.executable_ranges[0].file_offset);
    EXPECT_EQ(first.executable_ranges[0].virtual_address,
              resolved.executable_ranges[0].virtual_address);
    EXPECT_EQ(first.executable_ranges[0].file_size, resolved.executable_ranges[0].file_size);

    EXPECT_FALSE(
        dartplant::vm_abi::ResolveEngineIncarnationForAnchor(modules, 0x401100, &resolved));

    // Overlapping executable ownership is ambiguity, not a score/ranking tie.
    modules[1].executable_ranges[0] = first.executable_ranges[0];
    EXPECT_FALSE(
        dartplant::vm_abi::ResolveEngineIncarnationForAnchor(modules, 0x201100, &resolved));
}

TEST_CASE(VmAbiCandidateSelectionRequiresOneDistinctAbiIdentity) {
    const auto* dart344 = dartplant::FindRuntimeProfileByVersion(1);
    const auto* dart350 = dartplant::FindRuntimeProfileByVersion(2);
    EXPECT_TRUE(dart344 != nullptr);
    EXPECT_TRUE(dart350 != nullptr);

    dartplant::vm_abi::CandidateDiagnostic first{};
    first.profile = dart344;
    first.probe.passed = true;

    dartplant::vm_abi::CandidateDiagnostic alias = first;
    alias.snapshot_hash_match = true;
    std::vector<dartplant::vm_abi::CandidateDiagnostic> aliases = {first, alias};
    const auto alias_selection = dartplant::vm_abi::SelectUniquePassingCandidate(aliases);
    EXPECT_EQ(2U, alias_selection.passed_rows);
    EXPECT_EQ(1U, alias_selection.distinct_abis);
    EXPECT_TRUE(alias_selection.selected == &aliases[1]);

    dartplant::vm_abi::CandidateDiagnostic different{};
    different.profile = dart350;
    different.probe.passed = true;
    std::vector<dartplant::vm_abi::CandidateDiagnostic> ambiguous = {first, different};
    const auto ambiguous_selection = dartplant::vm_abi::SelectUniquePassingCandidate(ambiguous);
    EXPECT_EQ(2U, ambiguous_selection.passed_rows);
    EXPECT_EQ(2U, ambiguous_selection.distinct_abis);
    EXPECT_TRUE(ambiguous_selection.selected == nullptr);
}

TEST_CASE(VmGeneratedTransitionProofIsReadOnlyAndStateSensitive) {
    const auto* profile = dartplant::FindRuntimeProfileByVersion(1);
    EXPECT_TRUE(profile != nullptr);

    std::array<uint8_t, 0x1000> thread{};
    const auto write_word = [&](uint32_t offset, uint64_t value) {
        EXPECT_TRUE(static_cast<size_t>(offset) + sizeof(value) <= thread.size());
        std::memcpy(thread.data() + offset, &value, sizeof(value));
    };
    const auto& bridge = profile->thread_bridge;
    const auto& transition = profile->transition;
    write_word(bridge.execution_state_offset, transition.execution_generated);
    write_word(bridge.top_exit_frame_offset, transition.exit_none);
    write_word(bridge.vm_tag_offset, transition.vm_tag_dart);
    write_word(bridge.exit_through_ffi_offset, transition.exit_none);

    const uint64_t thread_address = reinterpret_cast<uint64_t>(thread.data());
    const auto generated =
        dartplant::vm_abi::ProveGeneratedTransitionState(*profile, thread_address);
    EXPECT_TRUE(generated.passed);
    EXPECT_EQ(transition.execution_generated, generated.execution_state);
    EXPECT_EQ(transition.vm_tag_dart, generated.vm_tag);

    write_word(bridge.execution_state_offset, transition.execution_native);
    write_word(bridge.top_exit_frame_offset, thread_address + 0x800);
    write_word(bridge.exit_through_ffi_offset, transition.exit_through_ffi);
    const auto native = dartplant::vm_abi::ProveGeneratedTransitionState(*profile, thread_address);
    EXPECT_FALSE(native.passed);
    EXPECT_EQ(transition.execution_native, native.execution_state);
    EXPECT_EQ(transition.exit_through_ffi, native.exit_through_ffi);
}
