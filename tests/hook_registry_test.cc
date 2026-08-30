// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <type_traits>

#include "core/internal.h"
#include "dartplant/advanced/artifact.h"
#include "dartplant/native_api.h"
#include "runtime/default_runtime.h"
#include "runtime/runtime_internal.h"
#include "test_runner.h"

namespace {

static_assert(std::is_same_v<DartPlantHostHook, DartPlantNativeHook>);
static_assert(std::is_same_v<DartPlantHostUnhook, DartPlantNativeUnhook>);

int g_hook_calls = 0;
int g_unhook_calls = 0;
void* g_last_target = nullptr;

struct ContextHostState {
    int hook_calls = 0;
    int unhook_calls = 0;
    void* last_target = nullptr;
};

struct MutatingHostState {
    std::array<uint8_t, 4> original{};
    bool installed = false;
};

int FakeHook(void* target, void*, void** backup) {
    ++g_hook_calls;
    g_last_target = target;
    *backup = target;
    return 0;
}

int FakeUnhook(void* target) {
    ++g_unhook_calls;
    g_last_target = target;
    return 0;
}

int FakeHostHook(void*, void* target, void* replacement, void** backup) {
    return FakeHook(target, replacement, backup);
}

int FakeHostUnhook(void*, void* target) { return FakeUnhook(target); }

int ContextHook(void* user_data, void* target, void*, void** backup) {
    auto* state = static_cast<ContextHostState*>(user_data);
    if (state == nullptr || backup == nullptr) return -1;
    ++state->hook_calls;
    state->last_target = target;
    *backup = target;
    return 0;
}

int ContextUnhook(void* user_data, void* target) {
    auto* state = static_cast<ContextHostState*>(user_data);
    if (state == nullptr) return -1;
    ++state->unhook_calls;
    state->last_target = target;
    return 0;
}

int MutatingHook(void* user_data, void* target, void*, void** backup) {
    auto* state = static_cast<MutatingHostState*>(user_data);
    if (state == nullptr || target == nullptr || backup == nullptr) return -1;
    std::memcpy(state->original.data(), target, state->original.size());
    const std::array<uint8_t, 4> patch = {0xde, 0xad, 0xbe, 0xef};
    std::memcpy(target, patch.data(), patch.size());
    state->installed = true;
    *backup = target;
    return 0;
}

int MutatingUnhook(void* user_data, void* target) {
    auto* state = static_cast<MutatingHostState*>(user_data);
    if (state == nullptr || target == nullptr || !state->installed) return -1;
    std::memcpy(target, state->original.data(), state->original.size());
    state->installed = false;
    return 0;
}

int Replacement(int, int) { return 42; }

void ResetFakeHost() {
    dartplant_reset();
    g_hook_calls = 0;
    g_unhook_calls = 0;
    g_last_target = nullptr;
    const DartPlantHostApi api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = nullptr,
        .hook = FakeHostHook,
        .unhook = FakeHostUnhook,
        .hook_with_publication = nullptr,
    };
    dartplant::InstallHostApi(&api);
    dartplant::RefreshModules();
}

}  // namespace

TEST_CASE(Arm64ReturnGraphDoesNotClaimUnreachableSiblingRet) {
    // entry+0 branches over a sibling entry at +4. Linear scanning used to
    // claim both RETs; entry-reachable CFG ownership must claim only +12.
    const std::array<uint32_t, 4> code = {
        0x14000002U,  // b +8 -> logical +8
        0xd65f03c0U,  // sibling-only RET at +4
        0xd503201fU,  // nop
        0xd65f03c0U,  // current entry RET at +12
    };
    std::vector<dartplant::Arm64ReturnPatch> returns;
    EXPECT_TRUE(dartplant::CollectReachableArm64Returns(
        reinterpret_cast<const uint8_t*>(code.data()), sizeof(code), 0x1000, &returns));
    EXPECT_EQ(1U, returns.size());
    EXPECT_EQ(0x100cULL, static_cast<unsigned long long>(returns[0].address));
}

TEST_CASE(Arm64ReturnGraphKeepsSharedBodyRetWhenReachable) {
    // A conditional entry can legitimately converge with another entry/body.
    // Both reachable return paths belong to this invocation and must be patched.
    const std::array<uint32_t, 4> code = {
        0x54000040U,  // b.eq +8
        0xd65f03c0U,  // fallthrough RET
        0xd503201fU,  // nop
        0xd65f03c0U,  // taken-path RET
    };
    std::vector<dartplant::Arm64ReturnPatch> returns;
    EXPECT_TRUE(dartplant::CollectReachableArm64Returns(
        reinterpret_cast<const uint8_t*>(code.data()), sizeof(code), 0x2000, &returns));
    EXPECT_EQ(2U, returns.size());
    EXPECT_EQ(0x2004ULL, static_cast<unsigned long long>(returns[0].address));
    EXPECT_EQ(0x200cULL, static_cast<unsigned long long>(returns[1].address));
}

TEST_CASE(Arm64ReturnGraphAcceptsDartNoreturnTrapBranch) {
    // Mirrors the shape emitted for the real throwing P6 fixture: a normal
    // return path and an error path that calls a noreturn stub then executes
    // BRK if the stub ever returns. The trap is terminal and must not force a
    // fallthrough into the next Code object.
    const std::array<uint32_t, 5> code = {
        0x54000060U,  // b.eq +12 -> error call
        0xd503201fU,  // nop
        0xd65f03c0U,  // normal RET
        0x94000000U,  // bl (target value is irrelevant to intra-entry CFG)
        0xd4200000U,  // brk #0
    };
    std::vector<dartplant::Arm64ReturnPatch> returns;
    EXPECT_TRUE(dartplant::CollectReachableArm64Returns(
        reinterpret_cast<const uint8_t*>(code.data()), sizeof(code), 0x2800, &returns));
    EXPECT_EQ(1U, returns.size());
    EXPECT_EQ(0x2808ULL, static_cast<unsigned long long>(returns[0].address));
}

TEST_CASE(Arm64ReturnGraphFailsClosedOnIndirectOrTailExit) {
    const std::array<uint32_t, 2> indirect = {0xd61f0200U, 0xd65f03c0U};  // br x16
    std::vector<dartplant::Arm64ReturnPatch> returns;
    EXPECT_FALSE(dartplant::CollectReachableArm64Returns(
        reinterpret_cast<const uint8_t*>(indirect.data()), sizeof(indirect), 0x3000, &returns));

    const std::array<uint32_t, 2> tail = {0x14000002U, 0xd65f03c0U};  // b past range
    EXPECT_FALSE(dartplant::CollectReachableArm64Returns(
        reinterpret_cast<const uint8_t*>(tail.data()), sizeof(tail), 0x4000, &returns));
}

TEST_CASE(ManagedHookPatchFingerprintPreservesSiblingArtifactBytes) {
    dartplant_reset();
    const long page_size = sysconf(_SC_PAGESIZE);
    EXPECT_TRUE(page_size > 0);
    void* mapping = mmap(nullptr, static_cast<size_t>(page_size),
                         PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_TRUE(mapping != MAP_FAILED);
    auto* bytes = static_cast<uint8_t*>(mapping);
    for (size_t index = 0; index < 32; ++index) bytes[index] = static_cast<uint8_t>(index + 1);
    const std::string pristine = dartplant::FingerprintCode(bytes, 32);

    MutatingHostState host;
    const DartPlantHostApi api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &host,
        .hook = MutatingHook,
        .unhook = MutatingUnhook,
        .hook_with_publication = nullptr,
    };
    dartplant::InstallHostApi(&api);

    auto target = std::make_shared<dartplant::DartEntryTarget>();
    target->id = reinterpret_cast<uintptr_t>(bytes + 16);
    target->entry = target->id;
    target->code_size = 16;
    void* backup = nullptr;
    DartPlantHook* hook = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::InstallHook(target, reinterpret_cast<void*>(Replacement), &backup, &hook));
    EXPECT_TRUE(hook != nullptr);
    EXPECT_TRUE(dartplant::FingerprintCode(bytes, 32) != pristine);
    EXPECT_EQ(pristine, dartplant::FingerprintCodeWithManagedPatches(bytes, 32));

    // Even at a managed patch site, normalize only the exact bytes DartPlant's
    // backend installed. A third party taking over that byte must remain visible.
    bytes[16] = 0xaaU;
    EXPECT_TRUE(dartplant::FingerprintCodeWithManagedPatches(bytes, 32) != pristine);
    bytes[16] = 0xdeU;
    EXPECT_EQ(pristine, dartplant::FingerprintCodeWithManagedPatches(bytes, 32));

    // Normalization may erase only bytes DartPlant actually changed. A foreign
    // mutation elsewhere in the sibling span must still invalidate the hash.
    bytes[4] ^= 0x80U;
    EXPECT_TRUE(dartplant::FingerprintCodeWithManagedPatches(bytes, 32) != pristine);
    bytes[4] ^= 0x80U;

    EXPECT_EQ(DARTPLANT_OK, dartplant::RemoveHook(hook));
    dartplant::ReleaseHook(hook);
    dartplant_reset();
    EXPECT_EQ(0, munmap(mapping, static_cast<size_t>(page_size)));
}

TEST_CASE(ManagedHookPatchFingerprintUnwindsOverlappingSiblingHooksInReverseOrder) {
    dartplant_reset();
    const long page_size = sysconf(_SC_PAGESIZE);
    EXPECT_TRUE(page_size > 0);
    void* mapping = mmap(nullptr, static_cast<size_t>(page_size),
                         PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_TRUE(mapping != MAP_FAILED);
    auto* bytes = static_cast<uint8_t*>(mapping);
    for (size_t index = 0; index < 32; ++index) bytes[index] = static_cast<uint8_t>(0x40 + index);
    const std::string pristine = dartplant::FingerprintCode(bytes, 32);

    MutatingHostState first_state;
    MutatingHostState second_state;
    DartPlantHostApi first_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &first_state,
        .hook = MutatingHook,
        .unhook = MutatingUnhook,
        .hook_with_publication = nullptr,
    };
    DartPlantHostApi second_api = first_api;
    second_api.user_data = &second_state;
    dartplant::InstallHostApi(&first_api);

    auto first_target = std::make_shared<dartplant::DartEntryTarget>();
    first_target->id = reinterpret_cast<uintptr_t>(bytes + 16);
    first_target->entry = first_target->id;
    first_target->code_size = 16;
    void* first_backup = nullptr;
    DartPlantHook* first_hook = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::InstallHook(first_target, reinterpret_cast<void*>(Replacement),
                                     &first_backup, &first_hook));

    // The second sibling begins two bytes later, so its 4-byte fake backend
    // patch overlaps the last two bytes of the first backend patch. Its saved
    // "original" therefore contains the first hook's patched bytes.
    dartplant::InstallHostApi(&second_api);
    auto second_target = std::make_shared<dartplant::DartEntryTarget>();
    second_target->id = reinterpret_cast<uintptr_t>(bytes + 18);
    second_target->entry = second_target->id;
    second_target->code_size = 14;
    void* second_backup = nullptr;
    DartPlantHook* second_hook = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::InstallHook(second_target, reinterpret_cast<void*>(Replacement),
                                     &second_backup, &second_hook));

    EXPECT_TRUE(dartplant::FingerprintCode(bytes, 32) != pristine);
    EXPECT_EQ(pristine, dartplant::FingerprintCodeWithManagedPatches(bytes, 32));

    EXPECT_EQ(DARTPLANT_OK, dartplant::RemoveHook(second_hook));
    EXPECT_EQ(DARTPLANT_OK, dartplant::RemoveHook(first_hook));
    dartplant::ReleaseHook(second_hook);
    dartplant::ReleaseHook(first_hook);
    dartplant_reset();
    EXPECT_EQ(0, munmap(mapping, static_cast<size_t>(page_size)));
}

TEST_CASE(EntryTargetKindsDoNotCreateFalseSharedFunctionIdentity) {
    auto target = std::make_shared<dartplant::DartEntryTarget>();
    target->entry = 0x1000;
    target->id = 0x1000;
    target->reported_alias_count = 1;
    target->identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;

    dartplant::DartMethodIdentity normal = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "target",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    auto unchecked = normal;
    unchecked.entry_kind = DARTPLANT_ENTRY_UNCHECKED;
    auto monomorphic = normal;
    monomorphic.entry_kind = DARTPLANT_ENTRY_MONOMORPHIC;
    target->AddAlias(normal);
    target->AddAlias(unchecked);
    target->AddAlias(monomorphic);

    EXPECT_EQ(3U, target->KnownAliasCount());
    EXPECT_FALSE(target->IsShared());
    EXPECT_TRUE(target->HasProvenUniqueIdentity());

    auto other_function = normal;
    other_function.function_name = "otherTarget";
    target->AddAlias(other_function);
    EXPECT_TRUE(target->IsShared());
    EXPECT_FALSE(target->HasProvenUniqueIdentity());
}

TEST_CASE(EntryTargetsShareExactCodePayloadOwnership) {
    dartplant::DartEntryTargetRegistry registry;
    auto normal =
        registry.GetOrCreate(0x1020, 0x60, 0xabc, 1, DARTPLANT_CODE_IDENTITY_UNIQUE, 0x1000, 0x80);
    auto unchecked =
        registry.GetOrCreate(0x1040, 0x40, 0xabc, 1, DARTPLANT_CODE_IDENTITY_UNIQUE, 0x1000, 0x80);
    EXPECT_TRUE(normal != nullptr);
    EXPECT_TRUE(unchecked != nullptr);
    EXPECT_TRUE(normal != unchecked);
    EXPECT_TRUE(normal->payload == unchecked->payload);
    EXPECT_EQ(0x1000ULL, static_cast<unsigned long long>(normal->payload->start));
    EXPECT_EQ(0x80U, normal->payload->instructions_length);
    EXPECT_EQ(0x1080ULL, static_cast<unsigned long long>(normal->payload->end()));
    EXPECT_TRUE(normal->payload->Contains(normal->entry, normal->code_size));
    EXPECT_TRUE(unchecked->payload->Contains(unchecked->entry, unchecked->code_size));

    EXPECT_TRUE(registry.GetOrCreate(0x1060, 0x20, 0xabc, 1, DARTPLANT_CODE_IDENTITY_UNIQUE, 0x1000,
                                     0x90) == nullptr);

    // Once a physical entry is bound, later producers may add a previously
    // unknown Code* but must not rewrite its exact entry-to-payload range or
    // contradict an already-known Code identity.
    EXPECT_TRUE(registry.GetOrCreate(0x1020, 0x50, 0xabc, 1, DARTPLANT_CODE_IDENTITY_UNIQUE, 0x1000,
                                     0x80) == nullptr);
    EXPECT_TRUE(registry.GetOrCreate(0x1020, 0x60, 0xdef, 1, DARTPLANT_CODE_IDENTITY_UNIQUE, 0x1000,
                                     0x80) == nullptr);

    dartplant::DartEntryTargetRegistry artifact_first_registry;
    auto artifact = artifact_first_registry.GetOrCreate(
        0x2020, 0x60, 0, 1, DARTPLANT_CODE_IDENTITY_UNIQUE, 0x2000, 0x80);
    auto live = artifact_first_registry.GetOrCreate(0x2020, 0x60, 0x1234, 1,
                                                    DARTPLANT_CODE_IDENTITY_UNIQUE, 0x2000, 0x80);
    EXPECT_TRUE(artifact != nullptr);
    EXPECT_TRUE(live == artifact);
    EXPECT_EQ(0x1234ULL, static_cast<unsigned long long>(live->code_object));

    dartplant::DartEntryTargetRegistry legacy_registry;
    auto legacy = legacy_registry.GetOrCreate(0x3020, 0x60, 0, 1, DARTPLANT_CODE_IDENTITY_UNIQUE);
    EXPECT_TRUE(legacy != nullptr);
    EXPECT_EQ(0x3020ULL, static_cast<unsigned long long>(legacy->payload->start));
    EXPECT_TRUE(!legacy->payload->exact_identity);
    auto upgraded = legacy_registry.GetOrCreate(0x3020, 0x60, 0x5678, 1,
                                                DARTPLANT_CODE_IDENTITY_UNIQUE, 0x3000, 0x80);
    EXPECT_TRUE(upgraded == legacy);
    EXPECT_EQ(0x3000ULL, static_cast<unsigned long long>(upgraded->payload->start));
    EXPECT_EQ(0x80U, upgraded->payload->instructions_length);
    EXPECT_EQ(0x5678ULL, static_cast<unsigned long long>(upgraded->payload->code_object));
    EXPECT_TRUE(upgraded->payload->exact_identity);
    EXPECT_TRUE(legacy_registry.GetOrCreate(0x3020, 0x60, 0x5678, 1,
                                            DARTPLANT_CODE_IDENTITY_UNIQUE) == upgraded);
    EXPECT_TRUE(legacy_registry.GetOrCreate(0x3070, 0x20, 0, 1, DARTPLANT_CODE_IDENTITY_UNIQUE,
                                            0x3000, 0x90) == nullptr);

    dartplant::DartEntryTargetRegistry hooked_legacy_registry;
    auto hooked_legacy =
        hooked_legacy_registry.GetOrCreate(0x4020, 0x60, 0, 1, DARTPLANT_CODE_IDENTITY_UNIQUE);
    EXPECT_TRUE(hooked_legacy != nullptr);
    hooked_legacy->BindHookRecord(reinterpret_cast<DartPlantHook*>(1));
    EXPECT_TRUE(hooked_legacy_registry.GetOrCreate(0x4020, 0x60, 0x9abc, 1,
                                                   DARTPLANT_CODE_IDENTITY_UNIQUE, 0x4000,
                                                   0x80) == nullptr);
}

TEST_CASE(GenericHostApiRetainsBackendInstanceForInstalledHook) {
    dartplant_reset();
    ContextHostState first;
    ContextHostState second;
    const DartPlantHostApi first_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &first,
        .hook = ContextHook,
        .unhook = ContextUnhook,
        .hook_with_publication = nullptr,
    };
    const DartPlantHostApi second_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &second,
        .hook = ContextHook,
        .unhook = ContextUnhook,
        .hook_with_publication = nullptr,
    };
    EXPECT_EQ(DARTPLANT_OK, dartplant_install_host_api(&first_api));

    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();

    DartPlantAddressQuery query = {
        .struct_size = sizeof(query),
        .module_name = "libdartplant_fixture.so",
        .address = reinterpret_cast<uint64_t>(target),
        .address_kind = DARTPLANT_ADDRESS_RUNTIME,
        .code_size = 1,
        .expected_build_id = nullptr,
        .expected_fingerprint = nullptr,
    };
    void* backup = nullptr;
    DartPlantHook* hook = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant_hook_address(&query, reinterpret_cast<void*>(Replacement), &backup, &hook));
    EXPECT_EQ(1, first.hook_calls);
    EXPECT_EQ(0, second.hook_calls);

    // Replacing the process default backend must not change ownership of a
    // physical hook already installed by the first backend instance.
    EXPECT_EQ(DARTPLANT_OK, dartplant_install_host_api(&second_api));
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(hook));
    EXPECT_EQ(1, first.unhook_calls);
    EXPECT_EQ(0, second.unhook_calls);
    dartplant_release_hook(hook);
    dlclose(fixture);
}

TEST_CASE(SimpleInitOwnsRuntimeProfileAndBootstrapsFindLazily) {
    dartplant_reset();
    ContextHostState host_state;
    const DartPlantHostApi host_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &host_state,
        .hook = ContextHook,
        .unhook = ContextUnhook,
        .hook_with_publication = nullptr,
    };
    const DartPlantInitInfo init = {
        .struct_size = sizeof(DartPlantInitInfo),
        .version = DARTPLANT_INIT_API_VERSION,
        .host_api = &host_api,
        .artifact_bundle = nullptr,
        .app_module_name = nullptr,
        .runtime_module_name = nullptr,
    };
    EXPECT_EQ(DARTPLANT_OK, dartplant_init(&init));
    EXPECT_EQ(1, static_cast<int>(dartplant_is_initialized()));

    // No Flutter images exist in the host test process. The important public
    // contract is that find_method owns refresh/bootstrap and fails at runtime
    // readiness rather than asking the caller for LiveVmContext/Profile data.
    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:fixture/main.dart",
        .class_name = "Global",
        .function_name = "missing",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY, dartplant_find_method(&query, &method));
    EXPECT_TRUE(method == nullptr);

    dartplant_shutdown();
    EXPECT_EQ(0, static_cast<int>(dartplant_is_initialized()));
}

TEST_CASE(SimpleInitValidatesReconfigurationAndShutdownClearsOnlyCurrentHost) {
    dartplant_reset();
    ContextHostState first;
    ContextHostState second;
    const DartPlantHostApi first_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &first,
        .hook = ContextHook,
        .unhook = ContextUnhook,
        .hook_with_publication = nullptr,
    };
    const DartPlantHostApi second_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &second,
        .hook = ContextHook,
        .unhook = ContextUnhook,
        .hook_with_publication = nullptr,
    };
    DartPlantInitInfo init = {
        .struct_size = sizeof(DartPlantInitInfo),
        .version = DARTPLANT_INIT_API_VERSION,
        .host_api = &first_api,
        .artifact_bundle = nullptr,
        .app_module_name = nullptr,
        .runtime_module_name = nullptr,
    };
    EXPECT_EQ(DARTPLANT_OK, dartplant_init(&init));
    EXPECT_EQ(DARTPLANT_OK, dartplant_init(&init));

    DartPlantInitInfo conflicting_module = init;
    conflicting_module.app_module_name = "other-app.so";
    EXPECT_EQ(DARTPLANT_PROFILE_MISMATCH, dartplant_init(&conflicting_module));
    DartPlantInitInfo conflicting_host = init;
    conflicting_host.host_api = &second_api;
    EXPECT_EQ(DARTPLANT_PROFILE_MISMATCH, dartplant_init(&conflicting_host));

    DartPlantCompilerAbiEvidence evidence{};
    evidence.struct_size = sizeof(evidence);
    evidence.snapshot_hash = "init-compatible-artifact";
    evidence.app_build_id = "test-build";
    evidence.code_fingerprint = "0000000000000000";
    evidence.result_representation = DARTPLANT_ABI_REPRESENTATION_TAGGED;
    evidence.library_uri = "package:init/main.dart";
    evidence.class_name = "Global";
    evidence.function_name = "lateArtifact";
    evidence.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    evidence.entry_va = 0x1000;
    evidence.code_size = 4;
    DartPlantArtifactBundle bundle{};
    bundle.struct_size = sizeof(bundle);
    bundle.version = DARTPLANT_ARTIFACT_BUNDLE_VERSION;
    bundle.compiler_abi_evidence = &evidence;
    bundle.compiler_abi_evidence_count = 1;
    DartPlantInitInfo compatible_artifact = init;
    compatible_artifact.artifact_bundle = &bundle;
    EXPECT_EQ(DARTPLANT_OK, dartplant_init(&compatible_artifact));

    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    DartPlantAddressQuery query = {
        .struct_size = sizeof(query),
        .module_name = "libdartplant_fixture.so",
        .address = reinterpret_cast<uint64_t>(target),
        .address_kind = DARTPLANT_ADDRESS_RUNTIME,
        .code_size = 1,
        .expected_build_id = nullptr,
        .expected_fingerprint = nullptr,
    };
    void* backup = nullptr;
    DartPlantHook* hook = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant_hook_address(&query, reinterpret_cast<void*>(Replacement), &backup, &hook));
    EXPECT_EQ(1, first.hook_calls);

    dartplant_shutdown();
    EXPECT_EQ(0, static_cast<int>(dartplant_is_initialized()));

    // The current default host was cleared, so a later null-host init cannot
    // silently reuse the borrowed first.user_data after its owner goes away.
    DartPlantInitInfo no_host = init;
    no_host.host_api = nullptr;
    EXPECT_EQ(DARTPLANT_HOST_API_UNAVAILABLE, dartplant_init(&no_host));

    // Clearing the process default must not break teardown of a physical hook
    // that retained the immutable binding that originally created it.
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(hook));
    EXPECT_EQ(1, first.unhook_calls);
    EXPECT_EQ(0, second.unhook_calls);
    dartplant_release_hook(hook);
    dlclose(fixture);
    dartplant_reset();
}

TEST_CASE(SimpleShutdownClearsHostBeforeConcurrentNullHostReinit) {
    dartplant_reset();
    ContextHostState first;
    ContextHostState second;
    const DartPlantHostApi first_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &first,
        .hook = ContextHook,
        .unhook = ContextUnhook,
        .hook_with_publication = nullptr,
    };
    const DartPlantHostApi second_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &second,
        .hook = ContextHook,
        .unhook = ContextUnhook,
        .hook_with_publication = nullptr,
    };
    const DartPlantInitInfo first_init = {
        .struct_size = sizeof(DartPlantInitInfo),
        .version = DARTPLANT_INIT_API_VERSION,
        .host_api = &first_api,
        .artifact_bundle = nullptr,
        .app_module_name = nullptr,
        .runtime_module_name = nullptr,
    };
    EXPECT_EQ(DARTPLANT_OK, dartplant_init(&first_init));
    DartPlantRuntime* runtime = dartplant::DefaultRuntimeInstanceForTesting();
    EXPECT_TRUE(runtime != nullptr);
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    EXPECT_TRUE(static_cast<bool>(operation));

    std::atomic_bool shutdown_done{false};
    std::thread shutdown_thread([&] {
        dartplant_shutdown();
        shutdown_done.store(true, std::memory_order_release);
    });

    // shutdown publishes runtime=null while its destroy is deliberately held
    // by operation. Once that state is visible, the old borrowed HostApi must
    // already have been removed under the same DefaultRuntime mutex.
    bool detached = false;
    for (int attempt = 0; attempt < 100000; ++attempt) {
        if (dartplant_is_initialized() == 0) {
            detached = true;
            break;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(detached);
    EXPECT_TRUE(!shutdown_done.load(std::memory_order_acquire));

    DartPlantInitInfo null_host_init = first_init;
    null_host_init.host_api = nullptr;
    EXPECT_EQ(DARTPLANT_HOST_API_UNAVAILABLE, dartplant_init(&null_host_init));

    operation = {};
    shutdown_thread.join();
    EXPECT_TRUE(shutdown_done.load(std::memory_order_acquire));

    DartPlantInitInfo second_init = first_init;
    second_init.host_api = &second_api;
    EXPECT_EQ(DARTPLANT_OK, dartplant_init(&second_init));
    dartplant_shutdown();
    dartplant_reset();
}

TEST_CASE(AddressHookUsesHostApiAndUnhooks) {
    ResetFakeHost();
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();

    DartPlantAddressQuery query = {
        .struct_size = sizeof(query),
        .module_name = "libdartplant_fixture.so",
        .address = reinterpret_cast<uint64_t>(target),
        .address_kind = DARTPLANT_ADDRESS_RUNTIME,
        .code_size = 1,
        .expected_build_id = nullptr,
        .expected_fingerprint = nullptr,
    };
    void* backup = nullptr;
    DartPlantHook* hook = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant_hook_address(&query, reinterpret_cast<void*>(Replacement), &backup, &hook));
    EXPECT_EQ(1, g_hook_calls);
    EXPECT_EQ(target, backup);
    EXPECT_EQ(target, g_last_target);
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(hook));
    EXPECT_EQ(1, g_unhook_calls);
    dartplant_release_hook(hook);
    dlclose(fixture);
}

TEST_CASE(MethodQueryResolvesFixtureAddress) {
    ResetFakeHost();
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto modules = dartplant::EnumerateModules();
    const auto module = dartplant::FindModule(modules, "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());

    const uint64_t elf_va = reinterpret_cast<uintptr_t>(target) - module->load_bias;
    const std::string metadata =
        "{\"format\":1,\"module\":{\"soname\":\"libdartplant_fixture.so\"},"
        "\"methods\":[{\"library_uri\":\"package:fixture/main.dart\","
        "\"class\":\"Fixture\",\"name\":\"add\","
        "\"entry_kind\":0,"
        "\"address_kind\":1,\"code_offset\":" +
        std::to_string(elf_va) + ",\"code_size\":1}]}";
    EXPECT_EQ(DARTPLANT_OK, dartplant_initialize_from_json(metadata.c_str()));

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "add",
        .signature = "(int, int) -> int",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_find_method(&query, &method));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(target), dartplant_method_runtime_address(method));
    dartplant_release_method(method);
    dartplant_reset();
    dlclose(fixture);
}

TEST_CASE(AddressHookRejectsNonExecutableRange) {
    ResetFakeHost();
    int stack_value = 0;
    DartPlantAddressQuery query = {
        .struct_size = sizeof(query),
        .module_name = "libc.so.6",
        .address = reinterpret_cast<uint64_t>(&stack_value),
        .address_kind = DARTPLANT_ADDRESS_RUNTIME,
        .code_size = sizeof(stack_value),
        .expected_build_id = nullptr,
        .expected_fingerprint = nullptr,
    };
    void* backup = nullptr;
    DartPlantHook* hook = nullptr;
    EXPECT_EQ(DARTPLANT_ADDRESS_OUTSIDE_EXECUTABLE,
              dartplant_hook_address(&query, reinterpret_cast<void*>(Replacement), &backup, &hook));
    EXPECT_EQ(0, g_hook_calls);
}
