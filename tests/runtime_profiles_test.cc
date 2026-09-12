// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "vm/runtime_profiles.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "test_runner.h"
#include "vm/abi/candidate_set.h"
#include "vm/abi/proof.h"
#include "vm/abi/resolver.h"

TEST_CASE(RuntimeProfilesMatchDartArm64CallingConvention) {
    constexpr std::array<uint8_t, 6> kExpectedGp = {1, 2, 3, 5, 6, 7};
    constexpr std::array<uint8_t, 6> kExpectedFpu = {0, 1, 2, 3, 4, 5};

    EXPECT_EQ(6U, dartplant::RuntimeProfileCount());
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
        EXPECT_EQ(std::string_view(profile.abi_id), std::string_view(profile.abi_identity.full));
        EXPECT_TRUE(profile.abi_identity.core != nullptr && profile.abi_identity.core[0] != '\0');
        EXPECT_TRUE(profile.abi_identity.call != nullptr && profile.abi_identity.call[0] != '\0');
        EXPECT_TRUE(profile.abi_identity.object != nullptr &&
                    profile.abi_identity.object[0] != '\0');
        EXPECT_TRUE(profile.abi_identity.transition != nullptr &&
                    profile.abi_identity.transition[0] != '\0');
        EXPECT_TRUE(profile.abi_identity.exception != nullptr &&
                    profile.abi_identity.exception[0] != '\0');
        EXPECT_EQ(8U, profile.machine.pointer_size);
        EXPECT_EQ(index < 3, profile.machine.product);
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
    facts.snapshot_features = "product arm64 android compressed-pointers";
    auto candidates = dartplant::ResolveRuntimeProfileCandidates(facts);
    EXPECT_EQ(3U, candidates.size());
    EXPECT_EQ(1U, candidates[0]->live_vm.profile_version);

    facts.snapshot_hash = "ffffffffffffffffffffffffffffffff";
    candidates = dartplant::ResolveRuntimeProfileCandidates(facts);
    EXPECT_EQ(3U, candidates.size());

    facts.snapshot_hash = {};
    candidates = dartplant::ResolveRuntimeProfileCandidates(facts);
    EXPECT_EQ(3U, candidates.size());

    facts.snapshot_hash = "d20a1be77c3d3c41b2a5accaee1ce549";
    facts.snapshot_features = "product arm64 android";
    candidates = dartplant::ResolveRuntimeProfileCandidates(facts);
    EXPECT_EQ(0U, candidates.size());
}

TEST_CASE(RuntimeProfileCandidatesDistinguishProductAndNonProductWithSameSnapshotHash) {
    constexpr std::string_view kHash = "d20a1be77c3d3c41b2a5accaee1ce549";
    const auto* ambiguous = dartplant::FindRuntimeProfileBySnapshot(kHash);
    EXPECT_TRUE(ambiguous == nullptr);
    const auto* product =
        dartplant::FindRuntimeProfileBySnapshot(kHash, "flutter-arm64-product-compressed");
    const auto* profile =
        dartplant::FindRuntimeProfileBySnapshot(kHash, "flutter-arm64-profile-compressed");
    EXPECT_TRUE(product != nullptr);
    EXPECT_TRUE(profile != nullptr);
    EXPECT_EQ(1U, product->live_vm.profile_version);
    EXPECT_EQ(4U, profile->live_vm.profile_version);
    EXPECT_TRUE(product->machine.product);
    EXPECT_FALSE(profile->machine.product);
    EXPECT_EQ(0x30U, product->live_vm.function_kind_tag_offset);
    EXPECT_EQ(0x34U, profile->live_vm.function_kind_tag_offset);
    EXPECT_EQ(product->thread_jump_to_frame_entry_point_offset,
              profile->thread_jump_to_frame_entry_point_offset);
    EXPECT_EQ(0U, dartplant::ThreadJumpToFrameOffsetForSnapshot(kHash));
    EXPECT_EQ(
        product->thread_jump_to_frame_entry_point_offset,
        dartplant::ThreadJumpToFrameOffsetForSnapshot(kHash, "flutter-arm64-product-compressed"));
    EXPECT_EQ(
        profile->thread_jump_to_frame_entry_point_offset,
        dartplant::ThreadJumpToFrameOffsetForSnapshot(kHash, "flutter-arm64-profile-compressed"));

    dartplant::VmRuntimeFacts facts{};
    facts.snapshot_hash = kHash;
    facts.snapshot_features = "release arm64 android compressed-pointers";
    const auto candidates = dartplant::ResolveRuntimeProfileCandidates(facts);
    EXPECT_EQ(3U, candidates.size());
    EXPECT_EQ(4U, candidates[0]->live_vm.profile_version);
    EXPECT_FALSE(candidates[0]->machine.product);
}

TEST_CASE(RuntimeProfileAbiIdentityTracksPrivateLayoutNotArtifactIdentity) {
    const auto* profiles = dartplant::RuntimeProfiles();
    EXPECT_EQ("dart-vm-arm64-product-compressed/abi-c43e7867dd748a37d2ceb96b",
              std::string_view(profiles[0].abi_identity.full));
    EXPECT_TRUE(std::string_view(profiles[0].abi_id) != std::string_view(profiles[1].abi_id));
    EXPECT_TRUE(std::string_view(profiles[0].abi_id) != std::string_view(profiles[2].abi_id));
    EXPECT_TRUE(std::string_view(profiles[1].abi_id) != std::string_view(profiles[2].abi_id));
    EXPECT_EQ("dart-vm-arm64-product-compressed/abi-core-e1061561f58d7c038f469012",
              std::string_view(profiles[0].abi_identity.core));
    EXPECT_EQ("dart-vm-arm64-product-compressed/abi-call-273aebdc747bafdd341f2e18",
              std::string_view(profiles[0].abi_identity.call));
    EXPECT_EQ("dart-vm-arm64-product-compressed/abi-object-d33d204969c0855db9521564",
              std::string_view(profiles[0].abi_identity.object));
    EXPECT_EQ("dart-vm-arm64-product-compressed/abi-transition-c7f4d6187f5f3a3fe8a521c7",
              std::string_view(profiles[0].abi_identity.transition));
    EXPECT_EQ("dart-vm-arm64-product-compressed/abi-exception-8dfdfa0cfa1b4e788769f837",
              std::string_view(profiles[0].abi_identity.exception));

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

TEST_CASE(RuntimeProfileDomainCandidateSelectionIsIndependent) {
    const auto* profiles = dartplant::RuntimeProfiles();
    dartplant::vm_abi::AbiCandidateSet candidates{};
    candidates.profiles = {&profiles[0], &profiles[1]};
    candidates.representative = &profiles[0];
    candidates.core_abi_id = profiles[0].abi_identity.core;

    const auto call = dartplant::vm_abi::SelectDomainAbi(
        candidates, dartplant::vm_abi::AbiDomain::kCall, {true, true});
    EXPECT_TRUE(call.passed());
    EXPECT_EQ(2U, call.compatible_rows);
    EXPECT_EQ(1U, call.distinct_abis);
    EXPECT_EQ(std::string_view(profiles[0].abi_identity.call), call.abi_id);

    const auto core = dartplant::vm_abi::SelectDomainAbi(
        candidates, dartplant::vm_abi::AbiDomain::kCore, {true, true});
    EXPECT_TRUE(core.ambiguous());
    EXPECT_TRUE(core.representative == nullptr);

    const auto partial = dartplant::vm_abi::SelectDomainAbi(
        candidates, dartplant::vm_abi::AbiDomain::kObject, {true, false});
    EXPECT_TRUE(partial.passed());
    EXPECT_TRUE(partial.representative == &profiles[0]);
}

TEST_CASE(RuntimeProfileDomainMutationPreservesUnrelatedDomains) {
    auto profiles = std::array<dartplant::RuntimeProfileRecord, 2>{dartplant::RuntimeProfiles()[0],
                                                                   dartplant::RuntimeProfiles()[0]};
    profiles[1].abi_identity.object = "mutated-object-domain";
    dartplant::vm_abi::AbiCandidateSet candidates{};
    candidates.profiles = {&profiles[0], &profiles[1]};
    candidates.representative = &profiles[0];
    candidates.core_abi_id = profiles[0].abi_identity.core;

    const auto object = dartplant::vm_abi::SelectDomainAbi(
        candidates, dartplant::vm_abi::AbiDomain::kObject, {true, true});
    EXPECT_TRUE(object.ambiguous());
    EXPECT_EQ(2U, object.distinct_abis);

    const auto transition = dartplant::vm_abi::SelectDomainAbi(
        candidates, dartplant::vm_abi::AbiDomain::kTransition, {true, true});
    EXPECT_TRUE(transition.passed());
    EXPECT_EQ(1U, transition.distinct_abis);
}

TEST_CASE(RuntimeProfileDependentDomainsRejectMixedCallAndObjectRows) {
    auto profiles = std::array<dartplant::RuntimeProfileRecord, 2>{dartplant::RuntimeProfiles()[0],
                                                                   dartplant::RuntimeProfiles()[0]};
    profiles[1].abi_identity.call = "mutated-call-domain";
    dartplant::vm_abi::AbiCandidateSet candidates{};
    candidates.profiles = {&profiles[0], &profiles[1]};
    candidates.representative = &profiles[0];
    candidates.core_abi_id = profiles[0].abi_identity.core;

    const auto arguments_domains = dartplant::vm_abi::CapabilityDomains(
        dartplant::vm_abi::kCapabilityArgumentsDescriptorLayout);
    EXPECT_TRUE((arguments_domains &
                 dartplant::vm_abi::AbiDomainBit(dartplant::vm_abi::AbiDomain::kCore)) == 0);
    EXPECT_TRUE((arguments_domains &
                 dartplant::vm_abi::AbiDomainBit(dartplant::vm_abi::AbiDomain::kCall)) != 0);
    EXPECT_TRUE((arguments_domains &
                 dartplant::vm_abi::AbiDomainBit(dartplant::vm_abi::AbiDomain::kObject)) != 0);
    const auto mixed =
        dartplant::vm_abi::SelectDomainAbiSet(candidates, arguments_domains, {true, true});
    EXPECT_TRUE(mixed.ambiguous());
    EXPECT_EQ(2U, mixed.distinct_domain_sets);

    const auto function_type_before_object_mutation = dartplant::vm_abi::SelectDomainAbiSet(
        candidates,
        dartplant::vm_abi::CapabilityDomains(dartplant::vm_abi::kCapabilityFunctionTypeLayout),
        {true, true});
    EXPECT_TRUE(function_type_before_object_mutation.passed());

    profiles[1].abi_identity.object = "mutated-object-domain";
    const auto function_type_after_object_mutation = dartplant::vm_abi::SelectDomainAbiSet(
        candidates,
        dartplant::vm_abi::CapabilityDomains(dartplant::vm_abi::kCapabilityFunctionTypeLayout),
        {true, true});
    EXPECT_TRUE(function_type_after_object_mutation.ambiguous());

    const auto unique =
        dartplant::vm_abi::SelectDomainAbiSet(candidates, arguments_domains, {true, false});
    EXPECT_TRUE(unique.passed());
    EXPECT_EQ(&profiles[0], unique.representative);
}

TEST_CASE(VmCapabilityRegistryDefinesColdAndEagerMasks) {
    EXPECT_EQ(17U, dartplant::vm_abi::CapabilityRegistrySize());
    uint64_t all = 0;
    for (size_t index = 0; index < dartplant::vm_abi::CapabilityRegistrySize(); ++index) {
        const auto& capability = dartplant::vm_abi::CapabilityRegistry()[index];
        EXPECT_TRUE(capability.capability != 0);
        EXPECT_EQ(0U, capability.capability & (capability.capability - 1));
        EXPECT_TRUE(capability.key != nullptr && capability.key[0] != '\0');
        EXPECT_TRUE(capability.diagnostic_name != nullptr && capability.diagnostic_name[0] != '\0');
        EXPECT_TRUE(dartplant::vm_abi::FindCapabilityDescriptor(capability.capability) ==
                    &capability);
        EXPECT_EQ(0U, all & capability.capability);
        all |= capability.capability;
    }
    EXPECT_EQ(UINT64_C(0x1ffff), all);
    EXPECT_EQ(UINT64_C(0xfff7), dartplant::vm_abi::ColdRequiredCapabilityMask());
    EXPECT_EQ(UINT64_C(0x237), dartplant::vm_abi::VerifiedAfterCreateCapabilityMask());
}

TEST_CASE(VmCapabilitySelectionRetainsEveryRowForTheSelectedKey) {
    auto profiles = std::array<dartplant::RuntimeProfileRecord, 2>{dartplant::RuntimeProfiles()[0],
                                                                   dartplant::RuntimeProfiles()[0]};
    dartplant::vm_abi::AbiCandidateSet candidates{};
    candidates.profiles = {&profiles[0], &profiles[1]};
    candidates.representative = &profiles[0];

    const auto selected = dartplant::vm_abi::SelectCapabilityAbiSet(
        candidates, dartplant::vm_abi::kCapabilityFunctionCodeLayout, {true, true});
    EXPECT_TRUE(selected.passed());
    EXPECT_EQ(2U, selected.compatible_rows);
    EXPECT_EQ(1U, selected.distinct_domain_sets);
    EXPECT_EQ(2U, selected.selected_rows.size());
    EXPECT_EQ(&profiles[0], selected.selected_rows[0]);
    EXPECT_EQ(&profiles[1], selected.selected_rows[1]);

    profiles[1].live_vm.code_entry_point_offset += sizeof(uint64_t);
    const auto ambiguous = dartplant::vm_abi::SelectCapabilityAbiSet(
        candidates, dartplant::vm_abi::kCapabilityFunctionCodeLayout, {true, true});
    EXPECT_TRUE(ambiguous.ambiguous());
    EXPECT_EQ(2U, ambiguous.compatible_rows);
    EXPECT_EQ(2U, ambiguous.distinct_domain_sets);
    EXPECT_TRUE(ambiguous.selected_rows.empty());
}

TEST_CASE(RuntimeProfileEagerCapabilityFingerprintsIgnoreUnconsumedDrift) {
    auto profiles = std::array<dartplant::RuntimeProfileRecord, 2>{dartplant::RuntimeProfiles()[0],
                                                                   dartplant::RuntimeProfiles()[0]};
    profiles[1].arguments_descriptor_register = 5;
    profiles[1].function_type.parameter_types_offset += 4;
    profiles[1].abi_identity.call = "mutated-call-domain";
    profiles[1].abi_identity.object = "mutated-object-domain";
    dartplant::vm_abi::AbiCandidateSet candidates{};
    candidates.profiles = {&profiles[0], &profiles[1]};
    candidates.representative = &profiles[0];

    for (uint64_t capability :
         {dartplant::vm_abi::kCapabilityRuntimeRoots, dartplant::vm_abi::kCapabilityOwnerIdentity,
          dartplant::vm_abi::kCapabilityCanonicalNull,
          dartplant::vm_abi::kCapabilityRegisterSemantics, dartplant::vm_abi::kCapabilityDartCore,
          dartplant::vm_abi::kCapabilitySafepointStubs}) {
        const auto selection =
            dartplant::vm_abi::SelectCapabilityAbiSet(candidates, capability, {true, true});
        EXPECT_TRUE(selection.passed());
    }
    EXPECT_TRUE(dartplant::vm_abi::SelectCapabilityAbiSet(
                    candidates, dartplant::vm_abi::kCapabilityInvocationCallAbi, {true, true})
                    .ambiguous());
    EXPECT_TRUE(dartplant::vm_abi::SelectCapabilityAbiSet(
                    candidates, dartplant::vm_abi::kCapabilityFunctionTypeLayout, {true, true})
                    .ambiguous());
}

TEST_CASE(RuntimeProfileFunctionTypeFingerprintCoversEveryConsumedField) {
    const auto& base = dartplant::RuntimeProfiles()[0];
    const auto capability = dartplant::vm_abi::kCapabilityFunctionTypeLayout;
    const auto baseline = dartplant::vm_abi::BuildCapabilityAbiKey(base, capability);
    const auto expect_change = [&](auto mutate) {
        auto profile = base;
        mutate(profile);
        EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(profile, capability) != baseline);
    };

    expect_change([](auto& p) { ++p.raw_object.heap_object_tag; });
    expect_change([](auto& p) { ++p.raw_object.smi_tag; });
    expect_change([](auto& p) { ++p.raw_object.smi_tag_mask; });
    expect_change([](auto& p) { ++p.raw_object.smi_tag_shift; });
    expect_change([](auto& p) { ++p.raw_object.class_id_tag_shift; });
    expect_change([](auto& p) { ++p.raw_object.class_id_tag_bits; });
    expect_change([](auto& p) { ++p.raw_object.compressed_word_size; });
    expect_change([](auto& p) { ++p.live_vm.thread_heap_base_offset; });
    expect_change([](auto& p) { ++p.function_type.function_signature_offset; });
    expect_change([](auto& p) { ++p.live_vm.array_length_offset; });
    expect_change([](auto& p) { ++p.live_vm.array_elements_offset; });
    expect_change([](auto& p) { ++p.live_vm.string_length_offset; });
    expect_change([](auto& p) { ++p.live_vm.string_data_offset; });
    expect_change([](auto& p) { ++p.live_vm.cid_function; });
    expect_change([](auto& p) { ++p.live_vm.cid_array; });
    expect_change([](auto& p) { ++p.live_vm.cid_immutable_array; });
    expect_change([](auto& p) { ++p.live_vm.cid_one_byte_string; });
    expect_change([](auto& p) { ++p.live_vm.cid_two_byte_string; });
    expect_change([](auto& p) { ++p.function_type.abstract_type_flags_offset; });
    expect_change([](auto& p) { ++p.function_type.result_type_offset; });
    expect_change([](auto& p) { ++p.function_type.parameter_types_offset; });
    expect_change([](auto& p) { ++p.function_type.named_parameter_names_offset; });
    expect_change([](auto& p) { ++p.function_type.packed_parameter_counts_offset; });
    expect_change([](auto& p) { ++p.function_type.packed_type_parameter_counts_offset; });
    expect_change([](auto& p) { ++p.function_type.cid_type; });
    expect_change([](auto& p) { ++p.function_type.cid_function_type; });
    expect_change([](auto& p) { ++p.function_type.cid_record_type; });
    expect_change([](auto& p) { ++p.function_type.cid_type_parameter; });
    expect_change([](auto& p) { ++p.function_type.cid_null; });
    expect_change([](auto& p) { ++p.function_type.cid_dynamic; });
    expect_change([](auto& p) { ++p.function_type.cid_void; });
    expect_change([](auto& p) { ++p.function_type.cid_never; });
    expect_change([](auto& p) { ++p.function_type.type_parameter_base_offset; });
    expect_change([](auto& p) { ++p.function_type.type_parameter_index_offset; });
    expect_change([](auto& p) { ++p.function_type.nullability_bits; });
    expect_change([](auto& p) { ++p.function_type.type_class_id_shift; });
    expect_change([](auto& p) { ++p.function_type.type_parameter_function_bit; });

    auto unconsumed = base;
    ++unconsumed.function_type.type_parameters_offset;
    EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(unconsumed, capability) == baseline);
}

TEST_CASE(RuntimeProfileArgumentsDescriptorFingerprintCoversObjectIdentityFields) {
    const auto& base = dartplant::RuntimeProfiles()[0];
    const auto capability = dartplant::vm_abi::kCapabilityArgumentsDescriptorLayout;
    const auto baseline = dartplant::vm_abi::BuildCapabilityAbiKey(base, capability);
    const auto function_type_baseline = dartplant::vm_abi::BuildCapabilityAbiKey(
        base, dartplant::vm_abi::kCapabilityFunctionTypeLayout);
    const auto expect_shared_change = [&](auto mutate) {
        auto profile = base;
        mutate(profile);
        EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(profile, capability) != baseline);
    };
    const auto expect_descriptor_change = [&](auto mutate) {
        auto profile = base;
        mutate(profile);
        EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(profile, capability) != baseline);
        EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(
                        profile, dartplant::vm_abi::kCapabilityFunctionTypeLayout) ==
                    function_type_baseline);
    };

    expect_shared_change([](auto& p) { ++p.raw_object.heap_object_tag; });
    expect_shared_change([](auto& p) { ++p.raw_object.smi_tag; });
    expect_shared_change([](auto& p) { ++p.raw_object.smi_tag_mask; });
    expect_shared_change([](auto& p) { ++p.raw_object.smi_tag_shift; });
    expect_shared_change([](auto& p) { ++p.raw_object.class_id_tag_shift; });
    expect_shared_change([](auto& p) { ++p.raw_object.class_id_tag_bits; });
    expect_shared_change([](auto& p) { ++p.raw_object.compressed_word_size; });
    expect_shared_change([](auto& p) { ++p.live_vm.thread_heap_base_offset; });
    expect_descriptor_change([](auto& p) { ++p.arguments_descriptor.type_args_len_offset; });
    expect_descriptor_change([](auto& p) { ++p.arguments_descriptor.count_offset; });
    expect_descriptor_change([](auto& p) { ++p.arguments_descriptor.size_offset; });
    expect_descriptor_change([](auto& p) { ++p.arguments_descriptor.positional_count_offset; });
    expect_descriptor_change([](auto& p) { ++p.arguments_descriptor.first_named_entry_offset; });
    expect_descriptor_change([](auto& p) { ++p.arguments_descriptor.named_entry_size; });
    expect_descriptor_change([](auto& p) { ++p.arguments_descriptor.name_offset; });
    expect_descriptor_change([](auto& p) { ++p.arguments_descriptor.position_offset; });
    expect_shared_change([](auto& p) { ++p.live_vm.cid_array; });
    expect_shared_change([](auto& p) { ++p.live_vm.cid_immutable_array; });
    expect_shared_change([](auto& p) { ++p.live_vm.array_length_offset; });
    expect_shared_change([](auto& p) { ++p.live_vm.array_elements_offset; });
    expect_shared_change([](auto& p) { ++p.live_vm.cid_one_byte_string; });
    expect_shared_change([](auto& p) { ++p.live_vm.cid_two_byte_string; });
    expect_shared_change([](auto& p) { ++p.live_vm.string_length_offset; });
    expect_shared_change([](auto& p) { ++p.live_vm.string_data_offset; });
}

TEST_CASE(RuntimeProfileTypeArgumentsFingerprintCoversElementStorage) {
    const auto& base = dartplant::RuntimeProfiles()[0];
    const auto capability = dartplant::vm_abi::kCapabilityTypeArgumentsLayout;
    const auto baseline = dartplant::vm_abi::BuildCapabilityAbiKey(base, capability);
    auto mutated = base;
    ++mutated.type_arguments.types_offset;
    EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(mutated, capability) != baseline);
}

TEST_CASE(RuntimeProfileFunctionCodeFingerprintIsComposedByCallAndAotCapabilities) {
    const auto& base = dartplant::RuntimeProfiles()[0];
    const auto function_code = dartplant::vm_abi::kCapabilityFunctionCodeLayout;
    const auto invocation = dartplant::vm_abi::kCapabilityInvocationCallAbi;
    const auto aot = dartplant::vm_abi::kCapabilityAotEntryLayout;
    const auto function_code_baseline =
        dartplant::vm_abi::BuildCapabilityAbiKey(base, function_code);
    const auto invocation_baseline = dartplant::vm_abi::BuildCapabilityAbiKey(base, invocation);
    const auto aot_baseline = dartplant::vm_abi::BuildCapabilityAbiKey(base, aot);

    const auto expect_composed_change = [&](auto mutate) {
        auto profile = base;
        mutate(profile);
        EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(profile, function_code) !=
                    function_code_baseline);
        EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(profile, invocation) !=
                    invocation_baseline);
        EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(profile, aot) != aot_baseline);
    };
    expect_composed_change([](auto& p) { ++p.live_vm.code_monomorphic_entry_point_offset; });
    expect_composed_change(
        [](auto& p) { ++p.live_vm.code_monomorphic_unchecked_entry_point_offset; });
}

TEST_CASE(RuntimeProfileExceptionBridgeFingerprintOwnsJumpTargetFields) {
    const auto& base = dartplant::RuntimeProfiles()[0];
    const auto capability = dartplant::vm_abi::kCapabilityExceptionBridgeLayout;
    const auto baseline = dartplant::vm_abi::BuildCapabilityAbiKey(base, capability);
    auto offset_mutated = base;
    ++offset_mutated.thread_jump_to_frame_entry_point_offset;
    EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(offset_mutated, capability) != baseline);
    auto code_entry_mutated = base;
    ++code_entry_mutated.live_vm.code_entry_point_offset;
    EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(code_entry_mutated, capability) !=
                baseline);
    auto active_exception_mutated = base;
    ++active_exception_mutated.thread_bridge.active_exception_offset;
    EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(active_exception_mutated, capability) ==
                baseline);
}

TEST_CASE(RuntimeProfileExceptionDomainsRemainIndependent) {
    const auto& base = dartplant::RuntimeProfiles()[0];
    const auto bridge = dartplant::vm_abi::kCapabilityExceptionBridgeLayout;
    const auto active = dartplant::vm_abi::kCapabilityExceptionLayout;
    auto profile = base;
    ++profile.thread_jump_to_frame_entry_point_offset;
    EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(profile, bridge) !=
                dartplant::vm_abi::BuildCapabilityAbiKey(base, bridge));
    EXPECT_TRUE(dartplant::vm_abi::BuildCapabilityAbiKey(profile, active) ==
                dartplant::vm_abi::BuildCapabilityAbiKey(base, active));
}

TEST_CASE(RuntimeProfileSafepointFingerprintOwnsStubTransitionFields) {
    auto profiles = std::array<dartplant::RuntimeProfileRecord, 2>{dartplant::RuntimeProfiles()[0],
                                                                   dartplant::RuntimeProfiles()[0]};
    dartplant::vm_abi::AbiCandidateSet candidates{};
    candidates.profiles = {&profiles[0], &profiles[1]};
    candidates.representative = &profiles[0];

    profiles[1].thread_bridge.enter_safepoint_stub_offset += 8;
    EXPECT_TRUE(dartplant::vm_abi::SelectCapabilityAbiSet(
                    candidates, dartplant::vm_abi::kCapabilitySafepointStubs, {true, true})
                    .ambiguous());
    profiles[1] = profiles[0];
    profiles[1].live_vm.code_entry_point_offset += 8;
    EXPECT_TRUE(dartplant::vm_abi::SelectCapabilityAbiSet(
                    candidates, dartplant::vm_abi::kCapabilitySafepointStubs, {true, true})
                    .ambiguous());
}

TEST_CASE(RuntimeProfileFunctionCodeEntryProofRejectsRelationalMutations) {
    const auto& profile = dartplant::RuntimeProfiles()[0];
    std::array<uint8_t, 0x400> heap{};
    const uint64_t heap_base = reinterpret_cast<uintptr_t>(heap.data());
    const uint64_t function = heap_base + 0x100 + profile.raw_object.heap_object_tag;
    const uint64_t code = heap_base + 0x200 + profile.raw_object.heap_object_tag;
    const uint64_t shared_function = heap_base + 0x300 + profile.raw_object.heap_object_tag;
    constexpr uint64_t kEntry = 0x12345678;

    auto write_word = [&heap, heap_base, &profile](uint64_t object, uint32_t offset,
                                                   uint64_t value) {
        std::memcpy(
            heap.data() + (object - profile.raw_object.heap_object_tag - heap_base) + offset,
            &value, sizeof(value));
    };
    auto write_compressed = [&heap, heap_base, &profile](uint64_t object, uint32_t offset,
                                                         uint64_t value) {
        const uint32_t compressed = static_cast<uint32_t>(value - heap_base);
        std::memcpy(
            heap.data() + (object - profile.raw_object.heap_object_tag - heap_base) + offset,
            &compressed, sizeof(compressed));
    };
    const uint64_t function_tags = uint64_t{profile.live_vm.cid_function}
                                   << profile.raw_object.class_id_tag_shift;
    const uint64_t code_tags = uint64_t{profile.live_vm.cid_code}
                               << profile.raw_object.class_id_tag_shift;
    std::memcpy(heap.data() + 0x100, &function_tags, sizeof(function_tags));
    std::memcpy(heap.data() + 0x200, &code_tags, sizeof(code_tags));
    std::memcpy(heap.data() + 0x300, &function_tags, sizeof(function_tags));
    write_word(function, profile.live_vm.function_entry_point_offset, kEntry);
    write_word(code, profile.live_vm.code_entry_point_offset, kEntry);
    write_compressed(function, profile.live_vm.function_code_offset, code);
    write_compressed(code, profile.live_vm.code_owner_offset, function);

    auto proof = dartplant::vm_abi::ProveFunctionCodeEntry(profile, heap_base, function, code,
                                                           kEntry, false);
    EXPECT_TRUE(proof.function_is_function);
    EXPECT_TRUE(proof.code_is_code);
    EXPECT_EQ(code, proof.function_code);
    EXPECT_EQ(function, proof.code_owner);
    EXPECT_EQ(kEntry, proof.function_entry);
    EXPECT_EQ(kEntry, proof.code_entry);
    EXPECT_TRUE(proof.passed);
    EXPECT_TRUE(proof.function_code_match);
    EXPECT_TRUE(proof.code_owner_match);

    write_compressed(code, profile.live_vm.code_owner_offset, shared_function);
    proof = dartplant::vm_abi::ProveFunctionCodeEntry(profile, heap_base, function, code, kEntry,
                                                      false);
    EXPECT_FALSE(proof.passed);
    proof =
        dartplant::vm_abi::ProveFunctionCodeEntry(profile, heap_base, function, code, kEntry, true);
    EXPECT_TRUE(proof.passed);

    constexpr uint64_t kUncheckedEntry = 0x123456a0;
    constexpr uint64_t kMonomorphicEntry = 0x123456b0;
    constexpr uint64_t kMonomorphicUncheckedEntry = 0x123456c0;
    write_word(function, profile.live_vm.function_unchecked_entry_point_offset, kUncheckedEntry);
    write_word(code, profile.live_vm.code_unchecked_entry_point_offset, kUncheckedEntry);
    write_word(code, profile.live_vm.code_monomorphic_entry_point_offset, kMonomorphicEntry);
    write_word(code, profile.live_vm.code_monomorphic_unchecked_entry_point_offset,
               kMonomorphicUncheckedEntry);
    proof = dartplant::vm_abi::ProveFunctionCodeEntry(
        profile, heap_base, function, code, kUncheckedEntry, true, DARTPLANT_ENTRY_UNCHECKED);
    EXPECT_TRUE(proof.passed);
    proof = dartplant::vm_abi::ProveFunctionCodeEntry(
        profile, heap_base, function, code, kMonomorphicEntry, true, DARTPLANT_ENTRY_MONOMORPHIC);
    EXPECT_TRUE(proof.passed);
    proof = dartplant::vm_abi::ProveFunctionCodeEntry(profile, heap_base, function, code,
                                                      kMonomorphicUncheckedEntry, true,
                                                      DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED);
    EXPECT_TRUE(proof.passed);
    proof = dartplant::vm_abi::ProveFunctionCodeEntry(profile, heap_base, function, code,
                                                      kUncheckedEntry, true);
    EXPECT_FALSE(proof.passed);

    write_compressed(function, profile.live_vm.function_code_offset, function);
    proof =
        dartplant::vm_abi::ProveFunctionCodeEntry(profile, heap_base, function, code, kEntry, true);
    EXPECT_FALSE(proof.passed);
}

TEST_CASE(RuntimeProfilePositiveCompressedSmiRejectsNegativeAndMalformedValues) {
    const auto& raw = dartplant::RuntimeProfiles()[0].raw_object;
    uint32_t decoded = UINT32_MAX;
    EXPECT_TRUE(dartplant::vm_abi::DecodePositiveCompressedSmi(14, raw, &decoded));
    EXPECT_EQ(7U, decoded);
    EXPECT_FALSE(dartplant::vm_abi::DecodePositiveCompressedSmi(UINT32_MAX - 1, raw, &decoded));
    EXPECT_FALSE(dartplant::vm_abi::DecodePositiveCompressedSmi(15, raw, &decoded));
    EXPECT_FALSE(dartplant::vm_abi::DecodePositiveCompressedSmi(14, raw, nullptr));
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
    modules[0].build_id.clear();
    EXPECT_FALSE(
        dartplant::vm_abi::ResolveEngineIncarnationForAnchor(modules, 0x201100, &resolved));
    modules[0].build_id = first.build_id;

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
