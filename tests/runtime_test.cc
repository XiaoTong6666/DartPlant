// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "dartplant/runtime.h"

#include <dlfcn.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "abi/calling_convention.h"
#include "core/internal.h"
#include "dartplant/advanced/artifact.h"
#include "dartplant/invocation.h"
#include "dartplant/runtime_profile.h"
#include "dartplant/vm_adapter.h"
#include "runtime/default_runtime.h"
#include "runtime/runtime_internal.h"
#include "test_runner.h"

namespace {

int Replacement(int left, int right) { return left + right + 10; }

int RetiredReplacement(int left, int right) { return left + right + 20; }

int RetiredReplacementLater(int left, int right) { return left + right + 30; }

int g_enter_calls = 0;
int g_leave_calls = 0;
uint32_t g_enter_depth = 0;
uint32_t g_leave_depth = 0;
DartPlantInvocation* g_last_invocation = nullptr;
std::vector<int> g_callback_order;
std::vector<std::string> g_requested_functions;
std::vector<uint8_t> g_requested_identity_ambiguous;
std::vector<uint32_t> g_requested_alias_counts;
std::vector<uint32_t> g_requested_known_alias_counts;
std::vector<uint8_t> g_logical_method_present;
std::vector<std::vector<std::string>> g_known_alias_functions;
DartPlantListener* g_self_removing_listener = nullptr;
int g_self_remove_leave_calls = 0;
int g_fake_unhook_calls = 0;
bool g_fake_fail_unhook_enabled = true;
int g_backend_a_unhook_calls = 0;
int g_backend_b_unhook_calls = 0;
DartPlantObjectHandle* g_object_argument_handle = nullptr;
uint64_t g_leave_x1 = 0;
uint64_t g_leave_v0 = 0;
DartPlantStatus g_callback_status = DARTPLANT_OK;
int g_late_shared_enter_calls = 0;
int g_late_shared_leave_calls = 0;
uint8_t g_late_shared_identity_ambiguous = 0;
uint8_t g_late_shared_verified_abi = 0;
DartPlantStatus g_late_shared_raw_status = DARTPLANT_NOT_INITIALIZED;
DartPlantStatus g_late_shared_typed_status = DARTPLANT_NOT_INITIALIZED;

struct FakeVmObject {
    uint64_t raw;
    DartPlantObjectKind kind;
    bool alive;
};

struct FakeVmState {
    int enters = 0;
    int leaves = 0;
    int scope_enters = 0;
    int scope_leaves = 0;
    int retains = 0;
    int releases = 0;
};

DartPlantStatus FakeEnter(void* user_data, const DartPlantIsolateIdentity*) {
    ++static_cast<FakeVmState*>(user_data)->enters;
    return DARTPLANT_OK;
}

DartPlantStatus FakeLeave(void* user_data, const DartPlantIsolateIdentity*) {
    ++static_cast<FakeVmState*>(user_data)->leaves;
    return DARTPLANT_OK;
}

DartPlantStatus FakeEnterScope(void* user_data, const DartPlantIsolateIdentity*) {
    ++static_cast<FakeVmState*>(user_data)->scope_enters;
    return DARTPLANT_OK;
}

DartPlantStatus FakeLeaveScope(void* user_data, const DartPlantIsolateIdentity*) {
    ++static_cast<FakeVmState*>(user_data)->scope_leaves;
    return DARTPLANT_OK;
}

DartPlantStatus FakeRetain(void* user_data, const DartPlantIsolateIdentity*, uint64_t raw,
                           DartPlantObjectStrength, void** out_handle) {
    ++static_cast<FakeVmState*>(user_data)->retains;
    *out_handle = new FakeVmObject{raw, DARTPLANT_OBJECT_OTHER, true};
    return DARTPLANT_OK;
}

DartPlantStatus FakeRelease(void* user_data, const DartPlantIsolateIdentity*, void* backend_handle,
                            DartPlantObjectStrength) {
    ++static_cast<FakeVmState*>(user_data)->releases;
    delete static_cast<FakeVmObject*>(backend_handle);
    return DARTPLANT_OK;
}

DartPlantStatus FakeKind(void*, const DartPlantIsolateIdentity*, void* backend_handle,
                         DartPlantObjectKind* out_kind) {
    *out_kind = static_cast<FakeVmObject*>(backend_handle)->kind;
    return DARTPLANT_OK;
}

DartPlantStatus FakeRaw(void*, const DartPlantIsolateIdentity*, void* backend_handle,
                        uint64_t* out_raw) {
    *out_raw = static_cast<FakeVmObject*>(backend_handle)->raw;
    return DARTPLANT_OK;
}

DartPlantStatus FakeAlive(void*, const DartPlantIsolateIdentity*, void* backend_handle,
                          uint8_t* out_alive) {
    *out_alive = static_cast<FakeVmObject*>(backend_handle)->alive;
    return DARTPLANT_OK;
}

void OnEnter(DartPlantInvocation* invocation, void*) {
    g_last_invocation = invocation;
    ++g_enter_calls;
    g_enter_depth = dartplant_invocation_depth(invocation);
    DartPlantValue value{};
    if (dartplant_invocation_get_argument(invocation, 0, &value) == DARTPLANT_OK) {
        value.raw += 10;
        dartplant_invocation_set_argument(invocation, 0, &value);
    }
}

void OnLeave(DartPlantInvocation* invocation, void*) {
    g_last_invocation = invocation;
    ++g_leave_calls;
    g_leave_depth = dartplant_invocation_depth(invocation);
    DartPlantValue value{};
    if (dartplant_invocation_get_result(invocation, &value) == DARTPLANT_OK) {
        value.raw += 100;
        dartplant_invocation_set_result(invocation, &value);
    }
}

void CaptureWideReturnRegisters(DartPlantInvocation* invocation, void*) {
    dartplant_invocation_get_gp_register(invocation, 1, &g_leave_x1);
    dartplant_invocation_get_fp_register(invocation, 0, &g_leave_v0);
}

void SkipWithResult(DartPlantInvocation* invocation, void*) {
    g_last_invocation = invocation;
    const DartPlantValue value = {DARTPLANT_VALUE_RAW_WORD, 0, 77};
    dartplant_invocation_set_result(invocation, &value);
}

void CallOriginal(DartPlantInvocation* invocation, void*) {
    g_last_invocation = invocation;
    g_callback_status = dartplant_invocation_call_original(invocation);
}

void RetainObjectAndSkip(DartPlantInvocation* invocation, void* user_data) {
    (void) user_data;
    dartplant_invocation_retain_argument_object(invocation, 0, DARTPLANT_OBJECT_STRONG,
                                                &g_object_argument_handle);
    dartplant_invocation_set_result_object(invocation, g_object_argument_handle);
}

void OrderedEnter(DartPlantInvocation*, void* user_data) {
    g_callback_order.push_back(*static_cast<int*>(user_data));
}

void OrderedLeave(DartPlantInvocation*, void* user_data) {
    g_callback_order.push_back(-*static_cast<int*>(user_data));
}

void LateSharedEnter(DartPlantInvocation* invocation, void*) {
    ++g_late_shared_enter_calls;
    g_late_shared_identity_ambiguous = dartplant_invocation_identity_ambiguous(invocation);
    g_late_shared_verified_abi = dartplant_invocation_has_verified_abi(invocation);
    uint64_t raw = 0;
    g_late_shared_raw_status = dartplant_invocation_get_gp_register(invocation, 1, &raw);
    DartPlantValue value{};
    g_late_shared_typed_status = dartplant_invocation_get_argument(invocation, 0, &value);
}

void LateSharedLeave(DartPlantInvocation*, void*) { ++g_late_shared_leave_calls; }

void RecordRequestedIdentity(DartPlantInvocation* invocation, void*) {
    g_last_invocation = invocation;
    const DartPlantMethod* method = dartplant_invocation_requested_method(invocation);
    g_requested_functions.push_back(method == nullptr ? std::string()
                                                      : method->record.function_name);
    g_requested_identity_ambiguous.push_back(dartplant_invocation_identity_ambiguous(invocation));
    g_requested_alias_counts.push_back(dartplant_invocation_code_alias_count(invocation));
    const uint32_t known_alias_count = dartplant_invocation_known_code_alias_count(invocation);
    g_requested_known_alias_counts.push_back(known_alias_count);
    g_logical_method_present.push_back(dartplant_invocation_logical_method(invocation) != nullptr);
    std::vector<std::string> aliases;
    for (uint32_t index = 0; index < known_alias_count; ++index) {
        DartPlantMethodIdentityInfo alias{};
        alias.struct_size = sizeof(alias);
        if (dartplant_invocation_get_code_alias(invocation, index, &alias) == DARTPLANT_OK) {
            aliases.emplace_back(alias.function_name == nullptr ? "" : alias.function_name);
        }
    }
    g_known_alias_functions.push_back(std::move(aliases));
}

void RemoveSelfOnEnter(DartPlantInvocation*, void*) {
    dartplant_remove_listener(g_self_removing_listener);
}

void SelfRemovedLeave(DartPlantInvocation*, void*) { ++g_self_remove_leave_calls; }

int FakeHook(void* target, void*, void** backup) {
    *backup = target;
    return 0;
}

int FakeUnhook(void*) {
    ++g_fake_unhook_calls;
    return 0;
}

int FakeFailUnhook(void*) {
    ++g_fake_unhook_calls;
    return g_fake_fail_unhook_enabled ? -1 : 0;
}

int BackendAUnhook(void*) {
    ++g_backend_a_unhook_calls;
    return 0;
}

int BackendBUnhook(void*) {
    ++g_backend_b_unhook_calls;
    return 0;
}

using TestHostHook = int (*)(void* target, void* replacement, void** backup);
using TestHostUnhook = int (*)(void* target);

struct TestHostBridge {
    TestHostHook hook = nullptr;
    TestHostUnhook unhook = nullptr;
};

int TestHostHookAdapter(void* user_data, void* target, void* replacement, void** backup) {
    const auto* bridge = static_cast<const TestHostBridge*>(user_data);
    return bridge == nullptr || bridge->hook == nullptr ? -1
                                                        : bridge->hook(target, replacement, backup);
}

int TestHostUnhookAdapter(void* user_data, void* target) {
    const auto* bridge = static_cast<const TestHostBridge*>(user_data);
    return bridge == nullptr || bridge->unhook == nullptr ? -1 : bridge->unhook(target);
}

void InstallTestHost(TestHostHook hook, TestHostUnhook unhook) {
    // HostApi.user_data is borrowed for as long as hooks created from the
    // binding can exist. Keep these tiny test bridges process-lifetime so test
    // order cannot leave the current default binding pointing at stack data.
    auto* bridge = new TestHostBridge{.hook = hook, .unhook = unhook};
    const DartPlantHostApi api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = bridge,
        .hook = TestHostHookAdapter,
        .unhook = TestHostUnhookAdapter,
    };
    dartplant::InstallHostApi(&api);
}

void SeedSyntheticLiveFunctionIndex(DartPlantRuntime* runtime, const dartplant::ModuleImage& module,
                                    void* target) {
    EXPECT_TRUE(runtime != nullptr);
    runtime->modules = dartplant::EnumerateModules();
    runtime->selected_app_module = module;
    runtime->selected_runtime_module = module;
    runtime->profile_matched = true;

    dartplant::FlutterSnapshotSource snapshot;
    snapshot.module_name = module.name;
    snapshot.module_path = module.path;
    snapshot.module_build_id = module.build_id;
    snapshot.snapshot_hash = "synthetic-live-index";
    snapshot.snapshot_features = "arm64 product compressed-pointers";
    snapshot.profile_name = "synthetic-live-index";
    snapshot.isolate_instructions_va = 0x1000;
    snapshot.isolate_instructions_size = 0x100;
    snapshot.isolate_instructions_runtime = reinterpret_cast<uintptr_t>(target);
    runtime->snapshot = std::move(snapshot);

    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    runtime->live_vm_context = context;

    dartplant::SnapshotIndex index;
    index.module_name = module.name;
    index.module_path = module.path;
    index.build_id = module.build_id;
    index.snapshot_hash = "synthetic-live-index";
    index.dart_version = "test";
    index.profile_version = "synthetic-live-index";
    dartplant::SnapshotFunction function;
    function.library_uri = "package:fixture/main.dart";
    function.class_name = "Fixture";
    function.function_name = "add";
    function.signature = "";
    function.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    function.code_size = 1;
    function.function_object = 0x1001;
    function.code_object = 0x2001;
    function.code_payload_start = reinterpret_cast<uintptr_t>(target);
    function.code_instructions_length = 1;
    function.runtime_entry = reinterpret_cast<uintptr_t>(target);
    function.code_entry = function.runtime_entry;
    function.entry_alias_count = 1;
    function.live = true;
    index.functions.push_back(std::move(function));
    runtime->live_snapshot_index = std::move(index);
    runtime->live_function_index_info.struct_size = sizeof(DartPlantLiveVmFunctionIndexInfo);
    runtime->live_function_index_info.function_count = 1;
    runtime->live_function_index_info.code_target_count = 1;
    runtime->state = DARTPLANT_RUNTIME_READY;
}

void SeedSyntheticArtifactImage(DartPlantRuntime* runtime, const dartplant::ModuleImage& module,
                                void* target, const char* snapshot_hash) {
    EXPECT_TRUE(runtime != nullptr);
    runtime->modules = dartplant::EnumerateModules();
    runtime->selected_app_module = module;
    runtime->selected_runtime_module = module;
    runtime->profile_matched = true;

    dartplant::FlutterSnapshotSource snapshot;
    snapshot.module_name = module.name;
    snapshot.module_path = module.path;
    snapshot.module_build_id = module.build_id;
    snapshot.snapshot_hash = snapshot_hash;
    snapshot.snapshot_features = "arm64 product compressed-pointers";
    snapshot.profile_name = "synthetic-artifact-index";
    snapshot.isolate_instructions_va = 0x1000;
    snapshot.isolate_instructions_size = 0x100;
    snapshot.isolate_instructions_runtime = reinterpret_cast<uintptr_t>(target);
    runtime->snapshot = std::move(snapshot);
    runtime->state = DARTPLANT_RUNTIME_IMAGES_READY;
}

}  // namespace

TEST_CASE(FunctionHandlesShareCodeTargetByEntry) {
    dartplant::DartCodeTargetRegistry registry;
    auto first_target = registry.GetOrCreate(0x123456, 76, 0x7300000011, 2);
    auto second_target = registry.GetOrCreate(0x123456, 76, 0x7300000011, 2);
    EXPECT_TRUE(first_target != nullptr);
    EXPECT_TRUE(first_target == second_target);

    dartplant::DartMethodIdentity first_identity = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Global",
        .function_name = "instrumentedAdd",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    dartplant::DartMethodIdentity second_identity = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "addInt",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    first_target->AddAlias(first_identity);
    second_target->AddAlias(second_identity);

    auto first_function = std::make_shared<dartplant::DartFunctionHandle>();
    first_function->identity = first_identity;
    first_function->function_object = 0x7300001011;
    first_function->code_object = 0x7300000011;
    first_function->source = dartplant::DartFunctionSource::kLiveVm;
    first_function->code_target = first_target;

    auto second_function = std::make_shared<dartplant::DartFunctionHandle>();
    second_function->identity = second_identity;
    second_function->function_object = 0x7300002011;
    second_function->code_object = 0x7300000011;
    second_function->source = dartplant::DartFunctionSource::kLiveVm;
    second_function->code_target = second_target;

    EXPECT_TRUE(first_function->code_target == second_function->code_target);
    EXPECT_TRUE(first_target->IsShared());
    EXPECT_EQ(2U, first_target->KnownAliasCount());
    EXPECT_TRUE(first_target->HasAlias(first_identity));
    EXPECT_TRUE(first_target->HasAlias(second_identity));

    DartPlantHook hook{};
    hook.code_target = first_target;
    first_target->BindHookRecord(&hook);
    EXPECT_TRUE(hook.code_target == first_function->code_target);
    EXPECT_TRUE(first_target->HookRecord() == &hook);
    first_target->UnbindHookRecord(&hook);
    EXPECT_TRUE(first_target->HookRecord() == nullptr);
}

TEST_CASE(SharedCodeCallbacksFailClosedAndExposeRequestedIdentity) {
    auto target = std::make_shared<dartplant::DartCodeTarget>();
    target->id = reinterpret_cast<uintptr_t>(Replacement);
    target->entry = reinterpret_cast<uintptr_t>(Replacement);
    target->code_size = 16;
    target->reported_alias_count = 2;

    auto first_function = std::make_shared<dartplant::DartFunctionHandle>();
    first_function->identity = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Global",
        .function_name = "instrumentedAdd",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    first_function->source = dartplant::DartFunctionSource::kSynthetic;
    first_function->code_target = target;
    auto second_function = std::make_shared<dartplant::DartFunctionHandle>();
    second_function->identity = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "addInt",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    second_function->source = dartplant::DartFunctionSource::kSynthetic;
    second_function->code_target = target;
    target->AddAlias(first_function->identity);
    target->AddAlias(second_function->identity);

    DartPlantMethod first{};
    first.record.function_name = "instrumentedAdd";
    first.function = first_function;
    DartPlantMethod second{};
    second.record.function_name = "addInt";
    second.function = second_function;

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT |
                    DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS | DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.shared_code_opt_in = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = target;
    hook.method_storage = std::make_unique<DartPlantMethod>(first);
    hook.profile = profile;
    hook.backup = reinterpret_cast<void*>(Replacement);

    DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = RecordRequestedIdentity,
        .on_leave = nullptr,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    DartPlantListener* rejected = nullptr;
    EXPECT_EQ(DARTPLANT_SHARED_CODE_ENTRY,
              dartplant::AddCallbackListener(&hook, &first, options, 0, &rejected));
    EXPECT_TRUE(rejected == nullptr);

    options.flags = DARTPLANT_HOOK_ALLOW_SHARED_CODE;
    DartPlantListener* first_listener = nullptr;
    DartPlantListener* second_listener = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::AddCallbackListener(&hook, &first, options, 0, &first_listener));
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::AddCallbackListener(&hook, &second, options, 0, &second_listener));

    g_requested_functions.clear();
    g_requested_identity_ambiguous.clear();
    g_requested_alias_counts.clear();
    g_requested_known_alias_counts.clear();
    g_logical_method_present.clear();
    g_known_alias_functions.clear();
    DartPlantArm64Context context{};
    const auto enter = dartplant_arm64_dispatch_enter(&context, &hook);
    EXPECT_EQ(reinterpret_cast<void*>(Replacement), enter.original);
    EXPECT_EQ(2U, g_requested_functions.size());
    EXPECT_EQ(std::string("instrumentedAdd"), g_requested_functions[0]);
    EXPECT_EQ(std::string("addInt"), g_requested_functions[1]);
    EXPECT_EQ(1U, static_cast<uint32_t>(g_requested_identity_ambiguous[0]));
    EXPECT_EQ(1U, static_cast<uint32_t>(g_requested_identity_ambiguous[1]));
    EXPECT_EQ(2U, g_requested_alias_counts[0]);
    EXPECT_EQ(2U, g_requested_alias_counts[1]);
    EXPECT_EQ(2U, g_requested_known_alias_counts[0]);
    EXPECT_EQ(2U, g_requested_known_alias_counts[1]);
    EXPECT_EQ(0U, static_cast<uint32_t>(g_logical_method_present[0]));
    EXPECT_EQ(0U, static_cast<uint32_t>(g_logical_method_present[1]));
    EXPECT_EQ(2U, g_known_alias_functions[0].size());
    EXPECT_EQ(std::string("instrumentedAdd"), g_known_alias_functions[0][0]);
    EXPECT_EQ(std::string("addInt"), g_known_alias_functions[0][1]);
    EXPECT_EQ(2U, g_known_alias_functions[1].size());
    EXPECT_EQ(std::string("instrumentedAdd"), g_known_alias_functions[1][0]);
    EXPECT_EQ(std::string("addInt"), g_known_alias_functions[1][1]);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(Replacement),
              dartplant_invocation_code_target_address(g_last_invocation));
    dartplant_arm64_dispatch_leave_from_tls(0, 0, 0);

    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(first_listener));
    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(second_listener));
    dartplant_release_listener(first_listener);
    dartplant_release_listener(second_listener);
}

TEST_CASE(RuntimeProfileStartsConservative) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    EXPECT_EQ(sizeof(DartPlantRuntimeProfile), profile.struct_size);
    EXPECT_EQ(1U, profile.profile_version);
    EXPECT_EQ(8U, profile.pointer_size);
    EXPECT_EQ(0U, profile.flags);
    EXPECT_EQ(std::string("libapp.so"), std::string(profile.app_module_name));
}

TEST_CASE(VmAdapterOwnsOpaqueObjectLifetime) {
    FakeVmState state{};
    const DartPlantVmAdapterCallbacks callbacks = {
        .struct_size = sizeof(DartPlantVmAdapterCallbacks),
        .adapter_version = 1,
        .enter_isolate = FakeEnter,
        .leave_isolate = FakeLeave,
        .enter_scope = FakeEnterScope,
        .leave_scope = FakeLeaveScope,
        .retain_object = FakeRetain,
        .release_object = FakeRelease,
        .object_kind = FakeKind,
        .object_to_raw = FakeRaw,
        .object_is_alive = FakeAlive,
    };
    DartPlantVmAdapter* adapter = nullptr;
    DartPlantObjectHandle* handle = nullptr;
    const DartPlantIsolateIdentity isolate = {1, 2, 3};
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_create(&callbacks, &state, &adapter));
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_attach_isolate(adapter, &isolate));
    EXPECT_EQ(DARTPLANT_VM_SCOPE_REQUIRED,
              dartplant_object_retain(adapter, 0x101, DARTPLANT_OBJECT_STRONG, &handle));
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_enter_scope(adapter));
    EXPECT_EQ(DARTPLANT_OK,
              dartplant_object_retain(adapter, 0x101, DARTPLANT_OBJECT_STRONG, &handle));
    EXPECT_EQ(1, state.retains);
    uint64_t raw = 0;
    EXPECT_EQ(DARTPLANT_OK, dartplant_object_to_raw(handle, &raw));
    EXPECT_EQ(0x101ULL, raw);
    EXPECT_EQ(DARTPLANT_VM_ADAPTER_BUSY, dartplant_vm_adapter_destroy(adapter));
    EXPECT_EQ(DARTPLANT_OK, dartplant_object_release(handle));
    EXPECT_EQ(1, state.releases);
    EXPECT_EQ(DARTPLANT_OBJECT_HANDLE_INVALID, dartplant_object_is_alive(handle, nullptr));
    EXPECT_EQ(DARTPLANT_OBJECT_HANDLE_INVALID, dartplant_object_to_raw(handle, &raw));
    EXPECT_EQ(DARTPLANT_OBJECT_HANDLE_INVALID, dartplant_object_release(handle));
    EXPECT_EQ(1, state.releases);
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_leave_scope(adapter));
    EXPECT_EQ(1, state.scope_enters);
    EXPECT_EQ(1, state.scope_leaves);
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_detach_isolate(adapter, &isolate));
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_destroy(adapter));
}

TEST_CASE(VmAdapterRejectsWrongThreadAndIsolateGeneration) {
    FakeVmState state{};
    const DartPlantVmAdapterCallbacks callbacks = {
        .struct_size = sizeof(DartPlantVmAdapterCallbacks),
        .adapter_version = 1,
        .enter_isolate = FakeEnter,
        .leave_isolate = FakeLeave,
        .enter_scope = FakeEnterScope,
        .leave_scope = FakeLeaveScope,
        .retain_object = FakeRetain,
        .release_object = FakeRelease,
        .object_kind = FakeKind,
        .object_to_raw = FakeRaw,
        .object_is_alive = FakeAlive,
    };
    DartPlantVmAdapter* adapter = nullptr;
    DartPlantObjectHandle* handle = nullptr;
    const DartPlantIsolateIdentity isolate = {11, 12, 13};
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_create(&callbacks, &state, &adapter));
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_attach_isolate(adapter, &isolate));
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_enter_scope(adapter));
    EXPECT_EQ(DARTPLANT_OK,
              dartplant_object_retain(adapter, 0x201, DARTPLANT_OBJECT_WEAK, &handle));
    DartPlantStatus thread_status = DARTPLANT_OK;
    uint8_t alive = 0;
    std::thread other([&] { thread_status = dartplant_object_is_alive(handle, &alive); });
    other.join();
    EXPECT_EQ(DARTPLANT_VM_THREAD_MISMATCH, thread_status);
    const DartPlantIsolateIdentity wrong_isolate = {11, 12, 14};
    EXPECT_EQ(DARTPLANT_VM_ISOLATE_MISMATCH,
              dartplant_vm_adapter_detach_isolate(adapter, &wrong_isolate));
    EXPECT_EQ(DARTPLANT_OK, dartplant_object_release(handle));
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_leave_scope(adapter));
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_detach_isolate(adapter, &isolate));
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_destroy(adapter));
}

TEST_CASE(InvocationObjectBridgeUsesActiveVmScope) {
    FakeVmState state{};
    const DartPlantVmAdapterCallbacks callbacks = {
        .struct_size = sizeof(DartPlantVmAdapterCallbacks),
        .adapter_version = 1,
        .enter_isolate = FakeEnter,
        .leave_isolate = FakeLeave,
        .enter_scope = FakeEnterScope,
        .leave_scope = FakeLeaveScope,
        .retain_object = FakeRetain,
        .release_object = FakeRelease,
        .object_kind = FakeKind,
        .object_to_raw = FakeRaw,
        .object_is_alive = FakeAlive,
    };
    DartPlantVmAdapter* adapter = nullptr;
    const DartPlantIsolateIdentity isolate = {21, 22, 23};
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_create(&callbacks, &state, &adapter));
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_attach_isolate(adapter, &isolate));

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT |
                    DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS | DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    profile.argument_count = 1;
    profile.argument_locations[0] = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
    DartPlantMethod method{};
    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = std::make_shared<dartplant::DartCodeTarget>();
    hook.code_target->id = reinterpret_cast<uintptr_t>(Replacement);
    hook.code_target->entry = reinterpret_cast<uintptr_t>(Replacement);
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.profile = profile;
    hook.vm_adapter = adapter;
    hook.backup = reinterpret_cast<void*>(Replacement);
    const DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = RetainObjectAndSkip,
        .on_leave = nullptr,
        .user_data = nullptr,
        .vm_adapter = adapter,
    };
    DartPlantListener* listener = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant::AddCallbackListener(&hook, &method, options, 0, &listener));

    g_object_argument_handle = nullptr;
    DartPlantArm64Context context{};
    context.x[0] = 1;
    const auto result = dartplant_arm64_dispatch_enter(&context, &hook);
    EXPECT_EQ(nullptr, result.original);
    EXPECT_TRUE(g_object_argument_handle != nullptr);
    DartPlantObjectKind kind = DARTPLANT_OBJECT_UNKNOWN;
    EXPECT_EQ(DARTPLANT_OK, dartplant_object_kind(g_object_argument_handle, &kind));
    EXPECT_EQ(DARTPLANT_OBJECT_OTHER, kind);
    EXPECT_EQ(DARTPLANT_OK, dartplant_object_release(g_object_argument_handle));
    g_object_argument_handle = nullptr;
    EXPECT_EQ(0, state.enters);
    EXPECT_EQ(0, state.leaves);
    EXPECT_EQ(1, state.scope_enters);
    EXPECT_EQ(1, state.scope_leaves);
    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(listener));
    dartplant_release_listener(listener);
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_detach_isolate(adapter, &isolate));
    EXPECT_EQ(DARTPLANT_OK, dartplant_vm_adapter_destroy(adapter));
}

TEST_CASE(DartCallbackHookRejectsVmAdapterWithoutGcSafeNativeTransition) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);

    auto target = std::make_shared<dartplant::DartCodeTarget>();
    target->id = reinterpret_cast<uintptr_t>(Replacement);
    target->entry = reinterpret_cast<uintptr_t>(Replacement);
    target->identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
    auto function = std::make_shared<dartplant::DartFunctionHandle>();
    function->source = dartplant::DartFunctionSource::kLiveVm;
    function->code_target = std::move(target);
    DartPlantMethod method{};
    method.function = std::move(function);

    const DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = OnEnter,
        .on_leave = nullptr,
        .user_data = nullptr,
        // The guard is checked before the adapter can be retained or called.
        .vm_adapter = reinterpret_cast<DartPlantVmAdapter*>(uintptr_t{1}),
    };
    DartPlantHook* hook = nullptr;
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI,
              dartplant::InstallCallbackHook(&method, profile, options, 0, &hook, nullptr));
    EXPECT_EQ(nullptr, hook);
}

TEST_CASE(RuntimeRequiresMatchingAotModules) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant_fixture.so";
    profile.runtime_module_name = "libdartplant_fixture.so";

    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    EXPECT_TRUE(runtime != nullptr);
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY,
              dartplant_runtime_on_module_loaded(runtime, "", nullptr));
    dartplant_runtime_destroy(runtime);
}

TEST_CASE(RuntimeDiagnosticsReportStructuredModuleRejection) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant-definitely-missing-app.so";
    profile.runtime_module_name = "libdartplant-definitely-missing-runtime.so";

    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    DartPlantResolutionDiagnostics diagnostics{};
    diagnostics.struct_size = sizeof(diagnostics);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_resolution_diagnostics(runtime, &diagnostics));
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_RESOLVE_MODULE_SELECTION),
              static_cast<uint8_t>(diagnostics.stage));
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_RESOLVE_REJECTED),
              static_cast<uint8_t>(diagnostics.outcome));
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_REJECT_MODULE_NOT_FOUND),
              static_cast<uint8_t>(diagnostics.reject_reason));
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY, diagnostics.status);
    EXPECT_EQ(0U, diagnostics.module_candidate_count);
    EXPECT_TRUE(diagnostics.runtime_generation != 0);
    dartplant_runtime_destroy(runtime);
}

TEST_CASE(RuntimeDestroyWaitsForPinnedOperations) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));

    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    EXPECT_TRUE(static_cast<bool>(operation));
    std::atomic_bool destroyed{false};
    std::thread destroyer([&] {
        dartplant_runtime_destroy(runtime);
        destroyed.store(true, std::memory_order_release);
    });

    bool closing = false;
    for (int attempt = 0; attempt < 100000; ++attempt) {
        auto probe = dartplant::AcquireRuntimeOperation(runtime);
        if (!probe) {
            closing = true;
            break;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(closing);
    EXPECT_TRUE(!destroyed.load(std::memory_order_acquire));
    operation = {};
    destroyer.join();
    EXPECT_TRUE(destroyed.load(std::memory_order_acquire));
}

TEST_CASE(RuntimeHookMethodHandleKeepsOuterOperationPinnedAcrossHelpers) {
    InstallTestHost(FakeHook, FakeUnhook);

    DartPlantCompilerAbiEvidence unrelated_evidence{};
    unrelated_evidence.struct_size = sizeof(unrelated_evidence);
    unrelated_evidence.snapshot_hash = "outer-pin-unrelated";
    unrelated_evidence.app_build_id = "outer-pin-unrelated";
    unrelated_evidence.code_fingerprint = "0000000000000000";
    unrelated_evidence.result_representation = DARTPLANT_ABI_REPRESENTATION_TAGGED;
    unrelated_evidence.library_uri = "package:unrelated/main.dart";
    unrelated_evidence.class_name = "Global";
    unrelated_evidence.function_name = "unrelated";
    unrelated_evidence.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    unrelated_evidence.entry_va = 0x1000;
    unrelated_evidence.code_size = 4;
    DartPlantArtifactBundle unrelated_bundle{};
    unrelated_bundle.struct_size = sizeof(unrelated_bundle);
    unrelated_bundle.version = DARTPLANT_ARTIFACT_BUNDLE_VERSION;
    unrelated_bundle.compiler_abi_evidence = &unrelated_evidence;
    unrelated_bundle.compiler_abi_evidence_count = 1;
    EXPECT_EQ(DARTPLANT_OK, dartplant_register_embedded_artifact_bundle(&unrelated_bundle));

    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto modules = dartplant::EnumerateModules();
    const auto module = dartplant::FindModule(modules, "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant_fixture.so";
    profile.runtime_module_name = "libdartplant_fixture.so";
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    SeedSyntheticLiveFunctionIndex(runtime, *module, target);

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "add",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &query, &method));
    EXPECT_TRUE(method != nullptr);

    const DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = OnEnter,
        .on_leave = OnLeave,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    DartPlantStatus hook_status = DARTPLANT_OK;
    DartPlantHookHandle* handle = nullptr;

    // Hold runtime->mutex so hook_method_handle stops inside the first nested
    // helper after acquiring both its API-wide operation lease and the helper
    // lease. Seeing two active operations makes the outer pin an explicit,
    // deterministic part of this regression rather than relying on timing.
    std::unique_lock runtime_lock(runtime->mutex);
    std::thread hooker([&] {
        hook_status = dartplant_runtime_hook_method_handle(runtime, method, &options, &handle);
    });
    bool saw_outer_and_nested = false;
    for (int attempt = 0; attempt < 100000; ++attempt) {
        if (dartplant::RuntimeActiveOperationCountForTesting(runtime) >= 2) {
            saw_outer_and_nested = true;
            break;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(saw_outer_and_nested);

    std::atomic_bool destroyed{false};
    std::thread destroyer([&] {
        dartplant_runtime_destroy(runtime);
        destroyed.store(true, std::memory_order_release);
    });
    bool closing = false;
    for (int attempt = 0; attempt < 100000; ++attempt) {
        auto probe = dartplant::AcquireRuntimeOperation(runtime);
        if (!probe) {
            closing = true;
            break;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(closing);
    EXPECT_TRUE(!destroyed.load(std::memory_order_acquire));

    runtime_lock.unlock();
    hooker.join();
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY, hook_status);
    EXPECT_TRUE(handle == nullptr);
    destroyer.join();
    EXPECT_TRUE(destroyed.load(std::memory_order_acquire));

    dartplant_release_method(method);
    dlclose(fixture);
}

TEST_CASE(FailedRuntimeHookInvalidationRetainsTargetOwnership) {
    InstallTestHost(FakeHook, FakeFailUnhook);
    g_fake_unhook_calls = 0;
    g_fake_fail_unhook_enabled = true;

    auto generation = std::make_shared<std::atomic_uint64_t>(1);
    DartPlantHook* hook = nullptr;
    void* backup = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::InstallHook(reinterpret_cast<uintptr_t>(Replacement),
                                     reinterpret_cast<void*>(Replacement), &backup, &hook));
    hook->runtime_generation = generation;
    hook->expected_runtime_generation = 1;
    EXPECT_EQ(DARTPLANT_UNHOOK_FAILED, dartplant::InvalidateRuntimeHooks(generation));
    EXPECT_EQ(1, g_fake_unhook_calls);

    DartPlantHook* duplicate = nullptr;
    backup = nullptr;
    EXPECT_EQ(DARTPLANT_ALREADY_HOOKED,
              dartplant::InstallHook(reinterpret_cast<uintptr_t>(Replacement),
                                     reinterpret_cast<void*>(Replacement), &backup, &duplicate));
    EXPECT_TRUE(duplicate == nullptr);

    InstallTestHost(FakeHook, FakeUnhook);
    EXPECT_EQ(DARTPLANT_UNHOOK_FAILED, dartplant_unhook(hook));
    EXPECT_EQ(2, g_fake_unhook_calls);
    g_fake_fail_unhook_enabled = false;
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(hook));
    dartplant_release_hook(hook);
}

TEST_CASE(HookUnhooksWithItsInstallingBackend) {
    InstallTestHost(FakeHook, BackendAUnhook);
    g_backend_a_unhook_calls = 0;
    g_backend_b_unhook_calls = 0;

    DartPlantHook* hook = nullptr;
    void* backup = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::InstallHook(reinterpret_cast<uintptr_t>(Replacement),
                                     reinterpret_cast<void*>(Replacement), &backup, &hook));
    InstallTestHost(FakeHook, BackendBUnhook);
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(hook));
    EXPECT_EQ(1, g_backend_a_unhook_calls);
    EXPECT_EQ(0, g_backend_b_unhook_calls);
    dartplant_release_hook(hook);
}

TEST_CASE(RetiredRuntimeHookRetainsBackendOwnership) {
    InstallTestHost(FakeHook, FakeUnhook);
    g_fake_unhook_calls = 0;

    auto generation = std::make_shared<std::atomic_uint64_t>(1);
    DartPlantHook* hook = nullptr;
    void* backup = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::InstallHook(reinterpret_cast<uintptr_t>(RetiredReplacement),
                                     reinterpret_cast<void*>(Replacement), &backup, &hook));
    hook->runtime_generation = generation;
    hook->expected_runtime_generation = 1;

    dartplant::RetireRuntimeHooks(generation);
    EXPECT_TRUE(!hook->active.load(std::memory_order_acquire));
    EXPECT_EQ(0, g_fake_unhook_calls);
    EXPECT_EQ(DARTPLANT_UNHOOK_FAILED, dartplant_unhook(hook));
    EXPECT_EQ(0, g_fake_unhook_calls);

    DartPlantHook* duplicate = nullptr;
    backup = nullptr;
    EXPECT_EQ(DARTPLANT_ALREADY_HOOKED,
              dartplant::InstallHook(reinterpret_cast<uintptr_t>(RetiredReplacement),
                                     reinterpret_cast<void*>(Replacement), &backup, &duplicate));
    EXPECT_TRUE(duplicate == nullptr);
    dartplant_release_hook(hook);
    dartplant_reset();
    EXPECT_EQ(0, g_fake_unhook_calls);
}

TEST_CASE(RetiredRuntimeHookDoesNotBlockLaterGenerationInvalidation) {
    InstallTestHost(FakeHook, FakeUnhook);
    g_fake_unhook_calls = 0;

    auto generation = std::make_shared<std::atomic_uint64_t>(1);
    DartPlantHook* retired = nullptr;
    void* backup = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::InstallHook(reinterpret_cast<uintptr_t>(RetiredReplacementLater),
                                     reinterpret_cast<void*>(Replacement), &backup, &retired));
    retired->runtime_generation = generation;
    retired->expected_runtime_generation = 1;
    dartplant::RetireRuntimeHooks(generation);

    generation->store(2, std::memory_order_release);
    EXPECT_EQ(DARTPLANT_OK, dartplant::InvalidateRuntimeHooks(generation));
    EXPECT_EQ(0, g_fake_unhook_calls);
    dartplant_release_hook(retired);
    dartplant_reset();
}

TEST_CASE(RuntimeResolvesMethodFromLiveFunctionIndexAndUsesHostHook) {
    InstallTestHost(FakeHook, FakeUnhook);
    g_fake_unhook_calls = 0;

    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto modules = dartplant::EnumerateModules();
    const auto module = dartplant::FindModule(modules, "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());
    EXPECT_TRUE(!module->executable_ranges.empty());

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant_fixture.so";
    profile.runtime_module_name = "libdartplant_fixture.so";
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    SeedSyntheticLiveFunctionIndex(runtime, *module, target);

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "add",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &query, &method));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(target), dartplant_method_runtime_address(method));

    DartPlantResolutionDiagnostics diagnostics{};
    diagnostics.struct_size = sizeof(diagnostics);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_resolution_diagnostics(runtime, &diagnostics));
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_RESOLVE_COMPLETE),
              static_cast<uint8_t>(diagnostics.stage));
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_RESOLVE_RESOLVED),
              static_cast<uint8_t>(diagnostics.outcome));
    EXPECT_EQ(1U, diagnostics.function_candidate_count);
    EXPECT_EQ(1U, diagnostics.code_alias_count);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(target), diagnostics.selected_entry);

    DartPlantRuntimeInfo runtime_info{};
    runtime_info.struct_size = sizeof(runtime_info);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_info(runtime, &runtime_info));
    EXPECT_EQ(1U, static_cast<uint32_t>(runtime_info.live_function_index_ready));

    DartPlantHook* hook = nullptr;
    void* backup = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant_runtime_hook_method_raw(
                  runtime, method, reinterpret_cast<void*>(Replacement), &backup, &hook));
    EXPECT_EQ(target, backup);
    EXPECT_TRUE(method->function != nullptr);
    EXPECT_TRUE(method->function->code_target != nullptr);
    EXPECT_TRUE(method->function->code_target->HookRecord() == hook);
    EXPECT_TRUE(hook->code_target == method->function->code_target);
    diagnostics = {};
    diagnostics.struct_size = sizeof(diagnostics);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_resolution_diagnostics(runtime, &diagnostics));
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_RESOLVE_COMPLETE),
              static_cast<uint8_t>(diagnostics.stage));
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_RESOLVE_RESOLVED),
              static_cast<uint8_t>(diagnostics.outcome));
    const uint64_t generation = runtime->generation->load(std::memory_order_acquire);

    auto resolver_duplicate = *module;
    resolver_duplicate.path += ".resolver-copy";
    runtime->modules.push_back(std::move(resolver_duplicate));
    DartPlantMethod* duplicate_safe_method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &query, &duplicate_safe_method));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(target),
              dartplant_method_runtime_address(duplicate_safe_method));
    dartplant_release_method(duplicate_safe_method);

    dartplant::StartRuntimeModuleRefreshWorker(nullptr);
    uint64_t refresh_epoch = 0;
    uint64_t first_refresh_epoch = 0;
    {
        std::lock_guard lock(runtime->mutex);
        for (int event = 0; event < 32; ++event) {
            (void) event;
            refresh_epoch = dartplant::ScheduleRuntimeModuleRefresh();
            if (first_refresh_epoch == 0) first_refresh_epoch = refresh_epoch;
        }
    }
    EXPECT_TRUE(first_refresh_epoch != 0);
    EXPECT_TRUE(refresh_epoch != 0);
    EXPECT_EQ(DARTPLANT_OK, dartplant::WaitForRuntimeModuleRefresh(refresh_epoch));
    EXPECT_EQ(DARTPLANT_OK, dartplant::WaitForRuntimeModuleRefresh(first_refresh_epoch));
    EXPECT_EQ(generation, runtime->generation->load(std::memory_order_acquire));
    EXPECT_EQ(DARTPLANT_RUNTIME_READY, runtime->state);
    EXPECT_TRUE(runtime->live_snapshot_index.has_value());
    EXPECT_TRUE(hook->active.load(std::memory_order_acquire));
    EXPECT_EQ(0, g_fake_unhook_calls);

    auto ambiguous_modules = modules;
    auto duplicate_module = *module;
    duplicate_module.path += ".namespace-copy";
    ambiguous_modules.push_back(std::move(duplicate_module));
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY,
              dartplant::RefreshRuntimeModules(runtime, ambiguous_modules));
    EXPECT_EQ(generation + 1, runtime->generation->load(std::memory_order_acquire));
    EXPECT_EQ(DARTPLANT_RUNTIME_FAILED, runtime->state);
    EXPECT_TRUE(!hook->active.load(std::memory_order_acquire));
    EXPECT_TRUE(method->function->code_target->HookRecord() == nullptr);
    EXPECT_EQ(1, g_fake_unhook_calls);
    EXPECT_TRUE(!runtime->selected_app_module.has_value());
    EXPECT_TRUE(!runtime->selected_runtime_module.has_value());
    DartPlantHook* duplicate_hook = nullptr;
    backup = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant::InstallHook(reinterpret_cast<uintptr_t>(target),
                                                   reinterpret_cast<void*>(Replacement), &backup,
                                                   &duplicate_hook));
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(duplicate_hook));
    dartplant_release_hook(duplicate_hook);

    SeedSyntheticLiveFunctionIndex(runtime, *module, target);
    std::vector<dartplant::ModuleImage> missing_modules;
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY,
              dartplant::RefreshRuntimeModules(runtime, missing_modules));
    EXPECT_EQ(generation + 2, runtime->generation->load(std::memory_order_acquire));
    EXPECT_TRUE(!runtime->live_snapshot_index.has_value());
    EXPECT_TRUE(!hook->active.load(std::memory_order_acquire));
    EXPECT_EQ(2, g_fake_unhook_calls);
    EXPECT_TRUE(method->function->code_target->HookRecord() == nullptr);
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(hook));
    dartplant_release_hook(hook);

    SeedSyntheticLiveFunctionIndex(runtime, *module, target);
    DartPlantHook* stale_hook = nullptr;
    backup = nullptr;
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY,
              dartplant_runtime_hook_method_raw(
                  runtime, method, reinterpret_cast<void*>(Replacement), &backup, &stale_hook));
    EXPECT_TRUE(stale_hook == nullptr);
    dartplant_release_method(method);

    DartPlantMethod* destroy_method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &query, &destroy_method));
    DartPlantHook* destroy_hook = nullptr;
    backup = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_hook_method_raw(runtime, destroy_method,
                                                              reinterpret_cast<void*>(Replacement),
                                                              &backup, &destroy_hook));
    EXPECT_TRUE(destroy_hook->active.load(std::memory_order_acquire));
    dartplant_runtime_destroy(runtime);
    EXPECT_TRUE(!destroy_hook->active.load(std::memory_order_acquire));
    EXPECT_EQ(3, g_fake_unhook_calls);
    EXPECT_TRUE(destroy_method->function->code_target->HookRecord() == nullptr);
    dartplant_release_hook(destroy_hook);
    dartplant_release_method(destroy_method);
    const uint64_t after_destroy_epoch = dartplant::ScheduleRuntimeModuleRefresh();
    EXPECT_TRUE(after_destroy_epoch != 0);
    EXPECT_EQ(DARTPLANT_OK, dartplant::WaitForRuntimeModuleRefresh(after_destroy_epoch));
    dartplant_reset();
    dlclose(fixture);
}

TEST_CASE(RuntimeLiveIndexResolvesExactEntryKindsWithoutSplittingLogicalFunction) {
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto module =
        dartplant::FindModule(dartplant::EnumerateModules(), "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());
    const uintptr_t base = reinterpret_cast<uintptr_t>(target);
    EXPECT_TRUE(module->ContainsExecutable(base, 16));

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant_fixture.so";
    profile.runtime_module_name = "libdartplant_fixture.so";
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    SeedSyntheticLiveFunctionIndex(runtime, *module, target);

    auto& index = *runtime->live_snapshot_index;
    index.functions[0].entry_va = 0x1000;
    index.functions[0].code_size = 16;
    index.functions[0].code_payload_start = base;
    index.functions[0].code_instructions_length = 16;
    index.functions[0].entry_alias_count = 1;
    const auto append_entry = [&](DartPlantEntryKind kind, uintptr_t entry) {
        auto function = index.functions[0];
        function.entry_kind = kind;
        function.runtime_entry = entry;
        function.code_entry = entry;
        function.entry_va = 0x1000 + (entry - base);
        function.code_size = 16 - (entry - base);
        function.entry_alias_count = 1;
        index.functions.push_back(std::move(function));
    };
    // Dart permits entry caches to alias. Exercise that explicitly: normal and
    // unchecked resolve to the same physical target but remain one Function.
    append_entry(DARTPLANT_ENTRY_UNCHECKED, base);
    append_entry(DARTPLANT_ENTRY_MONOMORPHIC, base + 4);
    append_entry(DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED, base + 8);

    const auto resolve = [&](DartPlantEntryKind kind, uintptr_t expected) {
        const DartPlantMethodQuery query = {
            .struct_size = sizeof(DartPlantMethodQuery),
            .library_uri = "package:fixture/main.dart",
            .class_name = "Fixture",
            .function_name = "add",
            .signature = "",
            .entry_kind = kind,
        };
        DartPlantMethod* method = nullptr;
        EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &query, &method));
        EXPECT_TRUE(method != nullptr);
        EXPECT_EQ(expected, dartplant_method_runtime_address(method));
        return method;
    };

    DartPlantMethod* normal = resolve(DARTPLANT_ENTRY_DEFAULT, base);
    DartPlantMethod* unchecked = resolve(DARTPLANT_ENTRY_UNCHECKED, base);
    DartPlantMethod* monomorphic = resolve(DARTPLANT_ENTRY_MONOMORPHIC, base + 4);
    DartPlantMethod* monomorphic_unchecked =
        resolve(DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED, base + 8);
    EXPECT_TRUE(normal->function->code_target == unchecked->function->code_target);
    EXPECT_EQ(2U, normal->function->code_target->KnownAliasCount());
    EXPECT_FALSE(normal->function->code_target->IsShared());

    DartPlantLiveVmFunctionInfo info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_function_info(runtime, 0, &info));
    EXPECT_EQ(0x0fU, static_cast<uint32_t>(info.entry_kind_mask));
    EXPECT_EQ(base, info.code_entry_point);
    EXPECT_EQ(base, info.code_unchecked_entry_point);
    EXPECT_EQ(base + 4, info.code_monomorphic_entry_point);
    EXPECT_EQ(base + 8, info.code_monomorphic_unchecked_entry_point);
    EXPECT_EQ(1U, info.entry_alias_counts[DARTPLANT_ENTRY_DEFAULT]);
    EXPECT_EQ(1U, info.entry_alias_counts[DARTPLANT_ENTRY_UNCHECKED]);
    EXPECT_EQ(1U, runtime->live_function_index_info.function_count);

    DartPlantLiveVmFunctionInfo legacy_info{};
    legacy_info.struct_size = offsetof(DartPlantLiveVmFunctionInfo, entry_alias_counts);
    legacy_info.entry_alias_counts[0] = 0xfeedbeefU;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_function_info(runtime, 0, &legacy_info));
    EXPECT_EQ(offsetof(DartPlantLiveVmFunctionInfo, entry_alias_counts), legacy_info.struct_size);
    EXPECT_EQ(base, legacy_info.code_entry_point);
    EXPECT_EQ(16U, legacy_info.code_size);
    EXPECT_EQ(0xfeedbeefU, legacy_info.entry_alias_counts[0]);

    dartplant_release_method(normal);
    dartplant_release_method(unchecked);
    dartplant_release_method(monomorphic);
    dartplant_release_method(monomorphic_unchecked);
    dartplant_runtime_destroy(runtime);
    dlclose(fixture);
}

TEST_CASE(RuntimeArtifactIndexAndAbiEvidenceBindExactUncheckedEntry) {
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto module =
        dartplant::FindModule(dartplant::EnumerateModules(), "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant_fixture.so";
    profile.runtime_module_name = "libdartplant_fixture.so";
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    SeedSyntheticArtifactImage(runtime, *module, target, "entry-kind-artifact");

    const std::string fingerprint = dartplant::FingerprintCode(target, 1);
    DartPlantSnapshotFunctionInfo functions[2]{};
    for (auto& function : functions) {
        function.struct_size = sizeof(function);
        function.library_uri = "package:fixture/main.dart";
        function.class_name = "Fixture";
        function.function_name = "entryKindTarget";
        function.signature = "";
        function.entry_va = 0x1000;
        function.code_size = 1;
        function.code_section_va = 0x1000;
        function.fingerprint = fingerprint.c_str();
        function.code_identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
        function.physical_entry_alias_count = 1;
        function.function_kind = 0;
        function.closure_call_entry_only = 0;
    }
    functions[0].entry_kind = DARTPLANT_ENTRY_DEFAULT;
    functions[1].entry_kind = DARTPLANT_ENTRY_UNCHECKED;

    DartPlantSnapshotIndexInfo source{};
    source.struct_size = sizeof(source);
    source.module_name = module->name.c_str();
    source.module_build_id = module->build_id.c_str();
    source.snapshot_hash = "entry-kind-artifact";
    source.dart_version = "test";
    source.profile_version = "entry-kind-artifact";
    source.functions = functions;
    source.function_count = 2;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_register_snapshot_index(runtime, &source));

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "entryKindTarget",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_UNCHECKED,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &query, &method));
    EXPECT_TRUE(method != nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(target), dartplant_method_runtime_address(method));
    EXPECT_EQ(DARTPLANT_ENTRY_UNCHECKED, method->record.entry_kind);

    DartPlantCompilerAbiEvidence evidence{};
    evidence.struct_size = sizeof(evidence);
    evidence.snapshot_hash = "entry-kind-artifact";
    evidence.app_build_id = module->build_id.c_str();
    evidence.code_fingerprint = fingerprint.c_str();
    evidence.parameter_count = 0;
    evidence.result_representation = DARTPLANT_ABI_REPRESENTATION_TAGGED;
    evidence.library_uri = "package:fixture/main.dart";
    evidence.class_name = "Fixture";
    evidence.function_name = "entryKindTarget";
    evidence.entry_kind = DARTPLANT_ENTRY_UNCHECKED;
    evidence.entry_va = 0x1000;
    evidence.code_size = 1;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant_runtime_register_compiler_abi_evidence(runtime, method, &evidence));
    DartPlantMethodAbiInfo abi{};
    abi.struct_size = sizeof(abi);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_method_abi_info(runtime, method, &abi));
    EXPECT_EQ(DARTPLANT_METHOD_ABI_VERIFIED, abi.state);
    EXPECT_EQ(1U, static_cast<uint32_t>(abi.has_verified_call_layout));
    EXPECT_FALSE(method->function->code_target->IsShared());

    dartplant_release_method(method);
    dartplant_runtime_destroy(runtime);
    dlclose(fixture);
}

TEST_CASE(RuntimeArtifactSnapshotIndexBindsDroppedFunctionFailClosed) {
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto modules = dartplant::EnumerateModules();
    const auto module = dartplant::FindModule(modules, "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant_fixture.so";
    profile.runtime_module_name = "libdartplant_fixture.so";
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    SeedSyntheticLiveFunctionIndex(runtime, *module, target);

    const std::string fingerprint = dartplant::FingerprintCode(target, 1);
    DartPlantSnapshotFunctionInfo function{};
    function.struct_size = sizeof(function);
    function.library_uri = "package:fixture/main.dart";
    function.class_name = "Fixture";
    function.function_name = "dropped";
    function.signature = "";
    function.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    function.entry_va = 0x1000;
    function.code_size = 1;
    function.code_section_va = 0x1000;
    function.fingerprint = fingerprint.c_str();
    function.code_identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
    function.physical_entry_alias_count = 1;

    DartPlantSnapshotIndexInfo source{};
    source.struct_size = sizeof(source);
    source.module_name = module->name.c_str();
    source.module_build_id = module->build_id.c_str();
    source.snapshot_hash = "synthetic-live-index";
    source.dart_version = "test";
    source.profile_version = "artifact-test-v1";
    source.functions = &function;
    source.function_count = 1;

    const char* saved_build_id = source.module_build_id;
    source.module_build_id = "wrong-build-id";
    EXPECT_EQ(DARTPLANT_PROFILE_MISMATCH,
              dartplant_runtime_register_snapshot_index(runtime, &source));
    source.module_build_id = saved_build_id;

    const char* saved_fingerprint = function.fingerprint;
    function.fingerprint = "0000000000000000";
    EXPECT_EQ(DARTPLANT_FINGERPRINT_MISMATCH,
              dartplant_runtime_register_snapshot_index(runtime, &source));
    function.fingerprint = saved_fingerprint;

    function.physical_entry_alias_count = 0;
    EXPECT_EQ(DARTPLANT_METADATA_INVALID,
              dartplant_runtime_register_snapshot_index(runtime, &source));
    function.physical_entry_alias_count = 1;

    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_register_snapshot_index(runtime, &source));
    EXPECT_EQ(DARTPLANT_INVALID_ARGUMENT,
              dartplant_runtime_register_snapshot_index(runtime, &source));

    const DartPlantMethodQuery dropped_query = {
        .struct_size = sizeof(dropped_query),
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "dropped",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* dropped = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &dropped_query, &dropped));
    EXPECT_TRUE(dropped != nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(target), dartplant_method_runtime_address(dropped));
    EXPECT_TRUE(dropped->function != nullptr);
    EXPECT_TRUE(dropped->function->source == dartplant::DartFunctionSource::kOfflineSnapshotIndex);

    DartPlantCompilerAbiEvidence dropped_evidence{};
    dropped_evidence.struct_size = sizeof(dropped_evidence);
    dropped_evidence.snapshot_hash = "synthetic-live-index";
    dropped_evidence.app_build_id = module->build_id.c_str();
    dropped_evidence.code_fingerprint = fingerprint.c_str();
    dropped_evidence.parameter_representations = nullptr;
    dropped_evidence.parameter_count = 0;
    dropped_evidence.result_representation = DARTPLANT_ABI_REPRESENTATION_TAGGED;
    dropped_evidence.max_parameters_in_registers = 0;
    dropped_evidence.library_uri = "package:fixture/main.dart";
    dropped_evidence.class_name = "Fixture";
    dropped_evidence.function_name = "dropped";
    dropped_evidence.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    dropped_evidence.entry_va = 0x1000;
    dropped_evidence.code_size = 1;
    dropped_evidence.structural_schema_version = 1;
    dropped_evidence.structural_decoded_instructions = 4;
    dropped_evidence.structural_basic_block_count = 1;
    dropped_evidence.structural_relation_count = 3;
    dropped_evidence.structural_verified = 1;
    dropped_evidence.structural_reached_return = 1;

    const char* saved_function_name = dropped_evidence.function_name;
    dropped_evidence.function_name = "different";
    EXPECT_EQ(DARTPLANT_PROFILE_MISMATCH, dartplant_runtime_register_compiler_abi_evidence(
                                              runtime, dropped, &dropped_evidence));
    dropped_evidence.function_name = saved_function_name;
    const uint64_t saved_entry_va = dropped_evidence.entry_va;
    dropped_evidence.entry_va = 0x1004;
    EXPECT_EQ(DARTPLANT_PROFILE_MISMATCH, dartplant_runtime_register_compiler_abi_evidence(
                                              runtime, dropped, &dropped_evidence));
    dropped_evidence.entry_va = saved_entry_va;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_register_compiler_abi_evidence(runtime, dropped,
                                                                             &dropped_evidence));
    DartPlantResolutionDiagnostics evidence_diagnostics{};
    evidence_diagnostics.struct_size = sizeof(evidence_diagnostics);
    EXPECT_EQ(DARTPLANT_OK,
              dartplant_runtime_get_resolution_diagnostics(runtime, &evidence_diagnostics));
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_RESOLVE_STRUCTURAL_EVIDENCE),
              static_cast<uint8_t>(evidence_diagnostics.stage));
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_RESOLVE_RESOLVED),
              static_cast<uint8_t>(evidence_diagnostics.outcome));
    EXPECT_EQ(1U, evidence_diagnostics.abi_provider_count);
    EXPECT_EQ(1U, evidence_diagnostics.structural_candidate_count);
    EXPECT_EQ(3U, evidence_diagnostics.structural_relation_count);
    DartPlantMethodAbiInfo dropped_abi{};
    dropped_abi.struct_size = sizeof(dropped_abi);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_method_abi_info(runtime, dropped, &dropped_abi));
    EXPECT_EQ(DARTPLANT_METHOD_ABI_VERIFIED, dropped_abi.state);
    EXPECT_EQ(0U, dropped_abi.parameter_count);
    EXPECT_EQ(1U, static_cast<uint32_t>(dropped_abi.has_verified_call_layout));
    EXPECT_TRUE(dropped->function->code_target->HasProvenUniqueIdentity());

    const DartPlantMethodQuery live_query = {
        .struct_size = sizeof(live_query),
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "add",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* live = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &live_query, &live));
    EXPECT_TRUE(live != nullptr);
    EXPECT_TRUE(live->function != nullptr);
    EXPECT_TRUE(live->function->source == dartplant::DartFunctionSource::kLiveVm);
    EXPECT_TRUE(dropped->function->code_target->IsShared());
    dropped_abi = {};
    dropped_abi.struct_size = sizeof(dropped_abi);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_method_abi_info(runtime, dropped, &dropped_abi));
    EXPECT_EQ(DARTPLANT_METHOD_ABI_UNSUPPORTED, dropped_abi.state);
    EXPECT_EQ(0U, static_cast<uint32_t>(dropped_abi.has_verified_call_layout));
    EXPECT_TRUE(dartplant::FindRuntimeCallLayoutLocked(runtime, dropped) == nullptr);
    dartplant_release_method(live);

    // Restore the test-only CodeTarget to its compiler-proven unique state so
    // the next registration reaches the formal-shape consistency check. A
    // later provider is not allowed to redefine the established dimension.
    {
        std::lock_guard target_lock(dropped->function->code_target->mutex);
        dropped->function->code_target->aliases.resize(1);
        dropped->function->code_target->reported_alias_count = 1;
        dropped->function->code_target->identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
    }
    const DartPlantAbiRepresentation one_parameter[] = {DARTPLANT_ABI_REPRESENTATION_TAGGED};
    dropped_evidence.parameter_representations = one_parameter;
    dropped_evidence.parameter_count = 1;
    dropped_evidence.max_parameters_in_registers = 1;
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI, dartplant_runtime_register_compiler_abi_evidence(
                                             runtime, dropped, &dropped_evidence));
    dropped_abi = {};
    dropped_abi.struct_size = sizeof(dropped_abi);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_method_abi_info(runtime, dropped, &dropped_abi));
    EXPECT_EQ(DARTPLANT_METHOD_ABI_CONFLICTING, dropped_abi.state);
    EXPECT_EQ(0U, static_cast<uint32_t>(dropped_abi.has_verified_call_layout));
    dartplant_release_method(dropped);

    EXPECT_TRUE(runtime->artifact_snapshot_index.has_value());
    runtime->artifact_snapshot_index->functions[0].fingerprint = "ffffffffffffffff";
    dropped = nullptr;
    EXPECT_EQ(DARTPLANT_FINGERPRINT_MISMATCH,
              dartplant_runtime_find_method(runtime, &dropped_query, &dropped));
    EXPECT_TRUE(dropped == nullptr);

    dartplant_runtime_destroy(runtime);
    dlclose(fixture);
}

TEST_CASE(EmbeddedArtifactRegistryOwnsDataAndMergesLateSnapshotBundles) {
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto module =
        dartplant::FindModule(dartplant::EnumerateModules(), "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant_fixture.so";
    profile.runtime_module_name = "libdartplant_fixture.so";
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    constexpr char kSnapshotHash[] = "artifact-owned-late-merge";
    SeedSyntheticArtifactImage(runtime, *module, target, kSnapshotHash);

    const std::string fingerprint = dartplant::FingerprintCode(target, 1);
    DartPlantSnapshotFunctionInfo first_function{};
    first_function.struct_size = sizeof(first_function);
    first_function.library_uri = "package:artifact_owned/main.dart";
    first_function.class_name = "Global";
    first_function.function_name = "lateOne";
    first_function.signature = "";
    first_function.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    first_function.entry_va = 0x1000;
    first_function.code_size = 1;
    first_function.code_section_va = 0x1000;
    first_function.fingerprint = fingerprint.c_str();
    first_function.code_identity_proof = DARTPLANT_CODE_IDENTITY_SHARED;
    first_function.physical_entry_alias_count = 2;
    DartPlantSnapshotIndexInfo first_index{};
    first_index.struct_size = sizeof(first_index);
    first_index.module_name = module->name.c_str();
    first_index.module_build_id = module->build_id.c_str();
    first_index.snapshot_hash = kSnapshotHash;
    first_index.dart_version = "test";
    first_index.profile_version = "owned-v1";
    first_index.functions = &first_function;
    first_index.function_count = 1;
    DartPlantArtifactBundle first_bundle{};
    first_bundle.struct_size = sizeof(first_bundle);
    first_bundle.version = DARTPLANT_ARTIFACT_BUNDLE_VERSION;
    first_bundle.snapshot_index = &first_index;
    EXPECT_EQ(DARTPLANT_OK, dartplant_register_embedded_artifact_bundle(&first_bundle));

    // Registration owns a deep copy. Mutating every caller-visible pointer
    // after the call must not alter the lazily consumed artifact.
    first_function.function_name = "corruptedAfterRegistration";
    first_index.snapshot_hash = "corrupted-after-registration";
    EXPECT_EQ(DARTPLANT_OK, dartplant::BindRegisteredArtifactIndexIfReady(runtime));

    const DartPlantMethodQuery first_query = {
        .struct_size = sizeof(first_query),
        .library_uri = "package:artifact_owned/main.dart",
        .class_name = "Global",
        .function_name = "lateOne",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* first_method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &first_query, &first_method));
    EXPECT_TRUE(first_method != nullptr);
    dartplant_release_method(first_method);

    DartPlantSnapshotFunctionInfo second_function{};
    second_function.struct_size = sizeof(second_function);
    second_function.library_uri = "package:artifact_owned/main.dart";
    second_function.class_name = "Global";
    second_function.function_name = "lateTwo";
    second_function.signature = "";
    second_function.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    second_function.entry_va = 0x1000;
    second_function.code_size = 1;
    second_function.code_section_va = 0x1000;
    second_function.fingerprint = fingerprint.c_str();
    second_function.code_identity_proof = DARTPLANT_CODE_IDENTITY_SHARED;
    second_function.physical_entry_alias_count = 2;
    DartPlantSnapshotIndexInfo second_index{};
    second_index.struct_size = sizeof(second_index);
    second_index.module_name = module->name.c_str();
    second_index.module_build_id = module->build_id.c_str();
    second_index.snapshot_hash = kSnapshotHash;
    second_index.dart_version = "test";
    second_index.profile_version = "owned-v1";
    second_index.functions = &second_function;
    second_index.function_count = 1;
    DartPlantArtifactBundle second_bundle{};
    second_bundle.struct_size = sizeof(second_bundle);
    second_bundle.version = DARTPLANT_ARTIFACT_BUNDLE_VERSION;
    second_bundle.snapshot_index = &second_index;
    EXPECT_EQ(DARTPLANT_OK, dartplant_register_embedded_artifact_bundle(&second_bundle));

    // A registry generation change must rebuild the already-bound merged index
    // instead of silently ignoring a sidecar from a later-loaded DSO.
    EXPECT_EQ(DARTPLANT_OK, dartplant::BindRegisteredArtifactIndexIfReady(runtime));
    EXPECT_TRUE(runtime->artifact_snapshot_index.has_value());
    EXPECT_EQ(2U, static_cast<uint32_t>(runtime->artifact_snapshot_index->functions.size()));
    const DartPlantMethodQuery second_query = {
        .struct_size = sizeof(second_query),
        .library_uri = "package:artifact_owned/main.dart",
        .class_name = "Global",
        .function_name = "lateTwo",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* second_method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &second_query, &second_method));
    EXPECT_TRUE(second_method != nullptr);
    dartplant_release_method(second_method);

    dartplant_runtime_destroy(runtime);
    dlclose(fixture);
}

TEST_CASE(EmbeddedArtifactRegistryAcceptsPreviousV2NestedStructSizes) {
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto module =
        dartplant::FindModule(dartplant::EnumerateModules(), "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant_fixture.so";
    profile.runtime_module_name = "libdartplant_fixture.so";
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    constexpr char kSnapshotHash[] = "artifact-v2-prefix-compat";
    SeedSyntheticArtifactImage(runtime, *module, target, kSnapshotHash);

    const std::string fingerprint = dartplant::FingerprintCode(target, 1);
    DartPlantSnapshotFunctionInfo function{};
    function.struct_size = offsetof(DartPlantSnapshotFunctionInfo, function_kind);
    function.library_uri = "package:artifact_v2/main.dart";
    function.class_name = "Global";
    function.function_name = "legacyV2";
    function.signature = "";
    function.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    function.entry_va = 0x1000;
    function.code_size = 1;
    function.code_section_va = 0x1000;
    function.fingerprint = fingerprint.c_str();
    function.code_identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
    function.physical_entry_alias_count = 1;

    DartPlantSnapshotIndexInfo index{};
    index.struct_size = sizeof(index);
    index.module_name = module->name.c_str();
    index.module_build_id = module->build_id.c_str();
    index.snapshot_hash = kSnapshotHash;
    index.dart_version = "legacy-v2";
    index.profile_version = "legacy-v2";
    index.functions = &function;
    index.function_count = 1;

    DartPlantCompilerAbiEvidence evidence{};
    evidence.struct_size = offsetof(DartPlantCompilerAbiEvidence, structural_schema_version);
    evidence.snapshot_hash = kSnapshotHash;
    evidence.app_build_id = module->build_id.c_str();
    evidence.code_fingerprint = fingerprint.c_str();
    evidence.result_representation = DARTPLANT_ABI_REPRESENTATION_TAGGED;
    evidence.library_uri = "package:artifact_v2/main.dart";
    evidence.class_name = "Global";
    evidence.function_name = "legacyV2";
    evidence.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    evidence.entry_va = 0x1000;
    evidence.code_size = 1;

    DartPlantArtifactBundle bundle{};
    bundle.struct_size = sizeof(bundle);
    bundle.version = DARTPLANT_ARTIFACT_BUNDLE_VERSION;
    bundle.snapshot_index = &index;
    bundle.compiler_abi_evidence = &evidence;
    bundle.compiler_abi_evidence_count = 1;
    EXPECT_EQ(DARTPLANT_OK, dartplant_register_embedded_artifact_bundle(&bundle));
    EXPECT_EQ(DARTPLANT_OK, dartplant::BindRegisteredArtifactIndexIfReady(runtime));

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:artifact_v2/main.dart",
        .class_name = "Global",
        .function_name = "legacyV2",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &query, &method));
    EXPECT_TRUE(method != nullptr);
    EXPECT_EQ(0U, method->function->function_kind);
    EXPECT_FALSE(method->function->closure_call_entry_only);
    DartPlantMethodAbiInfo abi{};
    abi.struct_size = sizeof(abi);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_method_abi_info(runtime, method, &abi));
    EXPECT_EQ(DARTPLANT_METHOD_ABI_VERIFIED, abi.state);

    dartplant_release_method(method);
    dartplant_runtime_destroy(runtime);
    dlclose(fixture);
}

TEST_CASE(EmbeddedArtifactEvidenceSkipsStaleSameNameBundle) {
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto module =
        dartplant::FindModule(dartplant::EnumerateModules(), "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant_fixture.so";
    profile.runtime_module_name = "libdartplant_fixture.so";
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    constexpr char kSnapshotHash[] = "artifact-evidence-selection";
    SeedSyntheticArtifactImage(runtime, *module, target, kSnapshotHash);

    const std::string fingerprint = dartplant::FingerprintCode(target, 1);
    DartPlantCompilerAbiEvidence stale_evidence{};
    stale_evidence.struct_size = sizeof(stale_evidence);
    stale_evidence.snapshot_hash = "older-app-incarnation";
    stale_evidence.app_build_id = module->build_id.c_str();
    stale_evidence.code_fingerprint = fingerprint.c_str();
    stale_evidence.result_representation = DARTPLANT_ABI_REPRESENTATION_TAGGED;
    stale_evidence.library_uri = "package:evidence_select/main.dart";
    stale_evidence.class_name = "Global";
    stale_evidence.function_name = "sameName";
    stale_evidence.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    stale_evidence.entry_va = 0x1000;
    stale_evidence.code_size = 1;
    DartPlantArtifactBundle stale_bundle{};
    stale_bundle.struct_size = sizeof(stale_bundle);
    stale_bundle.version = DARTPLANT_ARTIFACT_BUNDLE_VERSION;
    stale_bundle.compiler_abi_evidence = &stale_evidence;
    stale_bundle.compiler_abi_evidence_count = 1;
    EXPECT_EQ(DARTPLANT_OK, dartplant_register_embedded_artifact_bundle(&stale_bundle));

    DartPlantSnapshotFunctionInfo function{};
    function.struct_size = sizeof(function);
    function.library_uri = "package:evidence_select/main.dart";
    function.class_name = "Global";
    function.function_name = "sameName";
    function.signature = "";
    function.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    function.entry_va = 0x1000;
    function.code_size = 1;
    function.code_section_va = 0x1000;
    function.fingerprint = fingerprint.c_str();
    function.code_identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
    function.physical_entry_alias_count = 1;
    DartPlantSnapshotIndexInfo index{};
    index.struct_size = sizeof(index);
    index.module_name = module->name.c_str();
    index.module_build_id = module->build_id.c_str();
    index.snapshot_hash = kSnapshotHash;
    index.dart_version = "test";
    index.profile_version = "evidence-v1";
    index.functions = &function;
    index.function_count = 1;

    DartPlantCompilerAbiEvidence current_evidence = stale_evidence;
    current_evidence.snapshot_hash = kSnapshotHash;
    DartPlantArtifactBundle current_bundle{};
    current_bundle.struct_size = sizeof(current_bundle);
    current_bundle.version = DARTPLANT_ARTIFACT_BUNDLE_VERSION;
    current_bundle.snapshot_index = &index;
    current_bundle.compiler_abi_evidence = &current_evidence;
    current_bundle.compiler_abi_evidence_count = 1;
    EXPECT_EQ(DARTPLANT_OK, dartplant_register_embedded_artifact_bundle(&current_bundle));
    EXPECT_EQ(DARTPLANT_OK, dartplant::BindRegisteredArtifactIndexIfReady(runtime));

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:evidence_select/main.dart",
        .class_name = "Global",
        .function_name = "sameName",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &query, &method));
    EXPECT_TRUE(method != nullptr);
    DartPlantMethodAbiInfo abi{};
    abi.struct_size = sizeof(abi);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_method_abi_info(runtime, method, &abi));
    EXPECT_EQ(DARTPLANT_METHOD_ABI_VERIFIED, abi.state);
    EXPECT_EQ(1U, static_cast<uint32_t>(abi.has_verified_call_layout));

    dartplant_release_method(method);
    dartplant_runtime_destroy(runtime);
    dlclose(fixture);
}

TEST_CASE(EmbeddedArtifactEvidenceProcessesAllExactCandidatesAndFailsTypedConflictClosed) {
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto module =
        dartplant::FindModule(dartplant::EnumerateModules(), "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.app_module_name = "libdartplant_fixture.so";
    profile.runtime_module_name = "libdartplant_fixture.so";
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    constexpr char kSnapshotHash[] = "artifact-multi-evidence-conflict";
    SeedSyntheticArtifactImage(runtime, *module, target, kSnapshotHash);

    const std::string fingerprint = dartplant::FingerprintCode(target, 1);
    DartPlantSnapshotFunctionInfo function{};
    function.struct_size = sizeof(function);
    function.library_uri = "package:evidence_conflict/main.dart";
    function.class_name = "Global";
    function.function_name = "target";
    function.signature = "";
    function.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    function.entry_va = 0x1000;
    function.code_size = 1;
    function.code_section_va = 0x1000;
    function.fingerprint = fingerprint.c_str();
    function.code_identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
    function.physical_entry_alias_count = 1;
    DartPlantSnapshotIndexInfo index{};
    index.struct_size = sizeof(index);
    index.module_name = module->name.c_str();
    index.module_build_id = module->build_id.c_str();
    index.snapshot_hash = kSnapshotHash;
    index.dart_version = "test";
    index.profile_version = "multi-evidence-v1";
    index.functions = &function;
    index.function_count = 1;

    const auto make_evidence = [&](DartPlantAbiRepresentation result) {
        DartPlantCompilerAbiEvidence evidence{};
        evidence.struct_size = sizeof(evidence);
        evidence.snapshot_hash = kSnapshotHash;
        evidence.app_build_id = module->build_id.c_str();
        evidence.code_fingerprint = fingerprint.c_str();
        evidence.result_representation = result;
        evidence.library_uri = "package:evidence_conflict/main.dart";
        evidence.class_name = "Global";
        evidence.function_name = "target";
        evidence.entry_kind = DARTPLANT_ENTRY_DEFAULT;
        evidence.entry_va = 0x1000;
        evidence.code_size = 1;
        return evidence;
    };
    DartPlantCompilerAbiEvidence first = make_evidence(DARTPLANT_ABI_REPRESENTATION_TAGGED);
    DartPlantCompilerAbiEvidence second = make_evidence(DARTPLANT_ABI_REPRESENTATION_UNBOXED_INT64);

    DartPlantArtifactBundle first_bundle{};
    first_bundle.struct_size = sizeof(first_bundle);
    first_bundle.version = DARTPLANT_ARTIFACT_BUNDLE_VERSION;
    first_bundle.snapshot_index = &index;
    first_bundle.compiler_abi_evidence = &first;
    first_bundle.compiler_abi_evidence_count = 1;
    EXPECT_EQ(DARTPLANT_OK, dartplant_register_embedded_artifact_bundle(&first_bundle));

    DartPlantArtifactBundle second_bundle{};
    second_bundle.struct_size = sizeof(second_bundle);
    second_bundle.version = DARTPLANT_ARTIFACT_BUNDLE_VERSION;
    second_bundle.compiler_abi_evidence = &second;
    second_bundle.compiler_abi_evidence_count = 1;
    EXPECT_EQ(DARTPLANT_OK, dartplant_register_embedded_artifact_bundle(&second_bundle));
    EXPECT_EQ(DARTPLANT_OK, dartplant::BindRegisteredArtifactIndexIfReady(runtime));

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:evidence_conflict/main.dart",
        .class_name = "Global",
        .function_name = "target",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_find_method(runtime, &query, &method));
    EXPECT_TRUE(method != nullptr);
    DartPlantMethodAbiInfo abi{};
    abi.struct_size = sizeof(abi);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_method_abi_info(runtime, method, &abi));
    EXPECT_EQ(DARTPLANT_METHOD_ABI_CONFLICTING, abi.state);
    EXPECT_EQ(0U, static_cast<uint32_t>(abi.has_verified_call_layout));

    dartplant_release_method(method);
    dartplant_runtime_destroy(runtime);
    dlclose(fixture);
}

TEST_CASE(RuntimeLiveVmCaptureRejectsForeignAndStaleInvocation) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    DartPlantRuntime* first = nullptr;
    DartPlantRuntime* second = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &first));
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &second));

    dartplant::FlutterSnapshotSource snapshot;
    snapshot.module_name = "libapp.so";
    snapshot.module_path = "/data/app/libapp.so";
    snapshot.snapshot_hash = "capture-generation-test";
    first->snapshot = snapshot;
    second->snapshot = snapshot;

    DartPlantMethod method{};
    method.runtime_generation = first->generation;
    method.expected_runtime_generation = first->generation->load(std::memory_order_acquire);
    DartPlantHook hook{};
    hook.runtime_generation = first->generation;
    hook.expected_runtime_generation = method.expected_runtime_generation;
    DartPlantInvocation invocation{};
    invocation.requested_method = &method;
    invocation.hook = &hook;

    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY, dartplant_runtime_capture_live_vm(second, &invocation));
    first->generation->fetch_add(1, std::memory_order_acq_rel);
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY, dartplant_runtime_capture_live_vm(first, &invocation));

    dartplant_runtime_destroy(second);
    dartplant_runtime_destroy(first);
}

TEST_CASE(RuntimeMethodResolutionRequiresAutomaticLiveFunctionIndex) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    DartPlantRuntime* runtime = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_create(&profile, &runtime));
    runtime->profile_matched = true;
    runtime->state = DARTPLANT_RUNTIME_IMAGES_READY;

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "add",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY, dartplant_runtime_find_method(runtime, &query, &method));
    EXPECT_TRUE(method == nullptr);

    DartPlantRuntimeInfo info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(DARTPLANT_OK, dartplant_runtime_get_info(runtime, &info));
    EXPECT_EQ(0U, static_cast<uint32_t>(info.live_function_index_ready));
    dartplant_runtime_destroy(runtime);
}

TEST_CASE(InvocationDecodesAndEncodesValidatedDartNull) {
    constexpr uint64_t kCanonicalNull = 0x7300000001;
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT |
                    DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS | DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    profile.argument_count = 1;
    profile.argument_locations[0] = {DARTPLANT_ABI_GP_REGISTER, 1, {0, 0}};
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

    DartPlantArm64Context context{};
    context.x[0] = kCanonicalNull;
    context.x[1] = kCanonicalNull;
    DartPlantInvocation invocation{};
    invocation.profile = &profile;
    invocation.context = &context;
    invocation.validated_null_value = kCanonicalNull;

    DartPlantValue value{};
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_argument(&invocation, 0, &value));
    EXPECT_EQ(DARTPLANT_VALUE_NULL, value.kind);
    EXPECT_EQ(kCanonicalNull, value.raw);

    const DartPlantValue null_value = {DARTPLANT_VALUE_NULL, 0, 0};
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_set_argument(&invocation, 0, &null_value));
    EXPECT_EQ(kCanonicalNull, context.x[1]);
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_set_result(&invocation, &null_value));
    EXPECT_EQ(kCanonicalNull, context.x[0]);
    EXPECT_TRUE(invocation.skip_original);
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_result(&invocation, &value));
    EXPECT_EQ(DARTPLANT_VALUE_NULL, value.kind);

    DartPlantArm64Context raw_context{};
    raw_context.x[1] = kCanonicalNull;
    DartPlantInvocation raw_invocation{};
    raw_invocation.profile = &profile;
    raw_invocation.context = &raw_context;
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_argument(&raw_invocation, 0, &value));
    EXPECT_EQ(DARTPLANT_VALUE_HEAP_OBJECT, value.kind);
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI,
              dartplant_invocation_set_argument(&raw_invocation, 0, &null_value));
    EXPECT_EQ(kCanonicalNull, raw_context.x[1]);

    const DartPlantValue bool_value = {DARTPLANT_VALUE_BOOL, 0, 1};
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI,
              dartplant_invocation_set_argument(&invocation, 0, &bool_value));

    DartPlantRuntimeProfile raw_profile = profile;
    raw_profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT;
    DartPlantInvocation untagged_invocation{};
    untagged_invocation.profile = &raw_profile;
    untagged_invocation.context = &raw_context;
    untagged_invocation.validated_null_value = kCanonicalNull;
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_argument(&untagged_invocation, 0, &value));
    EXPECT_EQ(DARTPLANT_VALUE_RAW_WORD, value.kind);
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI,
              dartplant_invocation_set_argument(&untagged_invocation, 0, &null_value));
}

TEST_CASE(VerifiedCallLayoutDoesNotImplyCanonicalSemanticRoots) {
    constexpr uint64_t kUnvalidatedCanonicalObject = 0x7300000001;
    dartplant::abi::DartCallLayout layout;
    layout.parameters.resize(1);
    layout.parameters[0].representation = dartplant::abi::DartAbiRepresentation::kTagged;
    layout.parameters[0].location.count = 1;
    layout.parameters[0].location.locations[0] = {dartplant::abi::DartAbiLocationKind::kGpRegister,
                                                  1, 0};
    layout.result.representation = dartplant::abi::DartAbiRepresentation::kTagged;
    layout.result.location.count = 1;
    layout.result.location.locations[0] = {dartplant::abi::DartAbiLocationKind::kGpRegister, 0, 0};

    DartPlantArm64Context context{};
    context.x[0] = kUnvalidatedCanonicalObject;
    context.x[1] = kUnvalidatedCanonicalObject;
    DartPlantInvocation invocation{};
    invocation.call_layout = &layout;
    invocation.context = &context;
    // This models artifact-first installation in IMAGES_READY: transport is
    // compiler-verified, while LiveVm canonical NULL/Bool roots are still 0.
    EXPECT_EQ(1U, static_cast<uint32_t>(dartplant_invocation_has_verified_abi(&invocation)));

    DartPlantValue value{};
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_argument(&invocation, 0, &value));
    EXPECT_EQ(DARTPLANT_VALUE_HEAP_OBJECT, value.kind);
    EXPECT_EQ(kUnvalidatedCanonicalObject, value.raw);
    invocation.phase = DARTPLANT_INVOCATION_LEAVE;
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_result(&invocation, &value));
    EXPECT_EQ(DARTPLANT_VALUE_HEAP_OBJECT, value.kind);

    const DartPlantValue null_value = {DARTPLANT_VALUE_NULL, 0, 0};
    const DartPlantValue bool_value = {DARTPLANT_VALUE_BOOL, 0, 1};
    invocation.phase = DARTPLANT_INVOCATION_ENTER;
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI,
              dartplant_invocation_set_argument(&invocation, 0, &null_value));
    invocation.phase = DARTPLANT_INVOCATION_LEAVE;
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI, dartplant_invocation_set_result(&invocation, &bool_value));
    EXPECT_EQ(kUnvalidatedCanonicalObject, context.x[0]);
    EXPECT_EQ(kUnvalidatedCanonicalObject, context.x[1]);
}

TEST_CASE(InvocationDecodesAndEncodesValidatedCanonicalDartBool) {
    constexpr uint64_t kCanonicalTrue = 0x7300000011;
    constexpr uint64_t kCanonicalFalse = 0x7300000021;
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT |
                    DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS | DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    profile.argument_count = 1;
    profile.argument_locations[0] = {DARTPLANT_ABI_GP_REGISTER, 1, {0, 0}};
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

    DartPlantArm64Context context{};
    context.x[0] = kCanonicalFalse;
    context.x[1] = kCanonicalTrue;
    DartPlantInvocation invocation{};
    invocation.profile = &profile;
    invocation.context = &context;
    invocation.phase = DARTPLANT_INVOCATION_LEAVE;
    invocation.validated_bool_true_value = kCanonicalTrue;
    invocation.validated_bool_false_value = kCanonicalFalse;

    DartPlantValue value{};
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_argument(&invocation, 0, &value));
    EXPECT_EQ(DARTPLANT_VALUE_BOOL, value.kind);
    EXPECT_EQ(1ULL, value.raw);
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_result(&invocation, &value));
    EXPECT_EQ(DARTPLANT_VALUE_BOOL, value.kind);
    EXPECT_EQ(0ULL, value.raw);

    const DartPlantValue semantic_true = {DARTPLANT_VALUE_BOOL, 0, 1};
    const DartPlantValue semantic_false = {DARTPLANT_VALUE_BOOL, 0, 0};
    invocation.phase = DARTPLANT_INVOCATION_ENTER;
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_set_argument(&invocation, 0, &semantic_true));
    EXPECT_EQ(kCanonicalTrue, context.x[1]);
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_set_argument(&invocation, 0, &semantic_false));
    EXPECT_EQ(kCanonicalFalse, context.x[1]);

    invocation.phase = DARTPLANT_INVOCATION_LEAVE;
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_set_result(&invocation, &semantic_true));
    EXPECT_EQ(kCanonicalTrue, context.x[0]);
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_result(&invocation, &value));
    EXPECT_EQ(DARTPLANT_VALUE_BOOL, value.kind);
    EXPECT_EQ(1ULL, value.raw);

    const DartPlantValue invalid_bool = {DARTPLANT_VALUE_BOOL, 0, 2};
    EXPECT_EQ(DARTPLANT_PROFILE_MISMATCH,
              dartplant_invocation_set_result(&invocation, &invalid_bool));
    EXPECT_EQ(kCanonicalTrue, context.x[0]);

    DartPlantRuntimeProfile raw_profile = profile;
    raw_profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT;
    invocation.profile = &raw_profile;
    invocation.phase = DARTPLANT_INVOCATION_ENTER;
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_argument(&invocation, 0, &value));
    EXPECT_EQ(DARTPLANT_VALUE_RAW_WORD, value.kind);
    EXPECT_EQ(kCanonicalFalse, value.raw);
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI,
              dartplant_invocation_set_argument(&invocation, 0, &semantic_true));
    EXPECT_EQ(kCanonicalFalse, context.x[1]);

    auto generation = std::make_shared<std::atomic_uint64_t>(7);
    DartPlantHook hook{};
    hook.runtime_generation = generation;
    hook.expected_runtime_generation = 7;
    invocation.profile = &profile;
    invocation.hook = &hook;
    context.x[1] = kCanonicalTrue;
    generation->store(8, std::memory_order_release);
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_argument(&invocation, 0, &value));
    EXPECT_EQ(DARTPLANT_VALUE_HEAP_OBJECT, value.kind);
    EXPECT_EQ(kCanonicalTrue, value.raw);
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI,
              dartplant_invocation_set_argument(&invocation, 0, &semantic_false));
    EXPECT_EQ(kCanonicalTrue, context.x[1]);
}

TEST_CASE(InvocationRejectsUntypedProfile) {
    DartPlantArm64Context context{};
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    DartPlantInvocation invocation{};
    invocation.profile = &profile;
    invocation.context = &context;
    DartPlantValue value{};
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI, dartplant_invocation_get_argument(&invocation, 0, &value));
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI, dartplant_invocation_get_result(&invocation, &value));
}

TEST_CASE(VerifiedCallLayoutOverridesLegacyProfileWithTypedArm64Locations) {
    constexpr uint64_t kCanonicalNull = 0x7300000001;
    constexpr uint64_t kDoubleBits = 0x3ff8000000000000ULL;     // 1.5
    constexpr uint64_t kNewDoubleBits = 0x4004000000000000ULL;  // 2.5

    dartplant::abi::DartFunctionAbiResolution abi;
    abi.parameters = {
        {.representation = dartplant::abi::DartAbiRepresentation::kUnboxedInt64,
         .proof = dartplant::abi::DartAbiProofState::kProven},
        {.representation = dartplant::abi::DartAbiRepresentation::kUnboxedDouble,
         .proof = dartplant::abi::DartAbiProofState::kProven},
        {.representation = dartplant::abi::DartAbiRepresentation::kTagged,
         .proof = dartplant::abi::DartAbiProofState::kProven},
    };
    abi.result = {
        .representation = dartplant::abi::DartAbiRepresentation::kUnboxedDouble,
        .proof = dartplant::abi::DartAbiProofState::kProven,
    };
    abi.has_stack_calling_convention = true;
    abi.has_optional_parameter_info = true;
    abi.has_optional_parameters = false;
    abi.has_max_parameters_in_registers = true;
    abi.max_parameters_in_registers = 2;
    abi.fully_proven = true;

    dartplant::abi::DartCallLayout layout;
    EXPECT_TRUE(dartplant::abi::ComputeArm64AotCallLayout(abi, &layout));
    EXPECT_EQ(15u, layout.dart_sp_register);
    EXPECT_EQ(0, layout.parameters[2].location.locations[0].stack_offset);

    uint64_t entry_stack[] = {kCanonicalNull};
    DartPlantArm64Context context{};
    context.x[1] = static_cast<uint64_t>(-7LL);
    context.x[15] = reinterpret_cast<uintptr_t>(entry_stack);
    // Architectural SP is deliberately unrelated: verified Dart stack slots
    // use SPREG=x15, matching the compiler's x15/fp-relative locations.
    context.sp = 0x12345678;
    std::memcpy(context.v[0], &kDoubleBits, sizeof(kDoubleBits));

    DartPlantRuntimeProfile legacy_profile{};
    dartplant_runtime_profile_init_arm64_aot(&legacy_profile);
    // Keep the legacy profile deliberately untyped. The verified overlay must
    // be sufficient on its own.
    DartPlantInvocation invocation{};
    invocation.profile = &legacy_profile;
    invocation.call_layout = &layout;
    invocation.context = &context;
    invocation.validated_null_value = kCanonicalNull;

    EXPECT_EQ(3u, dartplant_invocation_argument_count(&invocation));
    DartPlantValue value{};
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_argument(&invocation, 0, &value));
    EXPECT_EQ(DARTPLANT_VALUE_INT64, value.kind);
    EXPECT_EQ(static_cast<uint64_t>(-7LL), value.raw);

    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_argument(&invocation, 1, &value));
    EXPECT_EQ(DARTPLANT_VALUE_DOUBLE, value.kind);
    EXPECT_EQ(kDoubleBits, value.raw);

    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_argument(&invocation, 2, &value));
    EXPECT_EQ(DARTPLANT_VALUE_NULL, value.kind);

    const DartPlantValue new_int = {DARTPLANT_VALUE_INT64, 0, 42};
    const DartPlantValue new_double = {DARTPLANT_VALUE_DOUBLE, 0, kNewDoubleBits};
    const DartPlantValue new_smi = {DARTPLANT_VALUE_SMI, 0, 0x20};
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_set_argument(&invocation, 0, &new_int));
    EXPECT_EQ(42ULL, context.x[1]);
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_set_argument(&invocation, 1, &new_double));
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_set_argument(&invocation, 2, &new_smi));
    EXPECT_EQ(0x20ULL, entry_stack[0]);

    invocation.phase = DARTPLANT_INVOCATION_LEAVE;
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_result(&invocation, &value));
    EXPECT_EQ(DARTPLANT_VALUE_DOUBLE, value.kind);
    EXPECT_EQ(kNewDoubleBits, value.raw);
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_set_result(&invocation, &new_double));
}

TEST_CASE(RuntimeAbiEvidenceBindingRequiresIdentityTargetGenerationAndUniqueCode) {
    DartPlantRuntime runtime{};
    DartPlantMethod method{};
    method.function = std::make_shared<dartplant::DartFunctionHandle>();
    method.function->identity = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "typed",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    method.function->code_target = std::make_shared<dartplant::DartCodeTarget>();
    method.function->code_target->id = 0x12340000;
    method.function->code_target->entry = 0x12340000;

    auto layout = std::make_shared<dartplant::abi::DartCallLayout>();
    layout->dart_sp_register = 15;
    dartplant::RuntimeAbiEvidenceEntry evidence_entry;
    evidence_entry.identity = method.function->identity;
    evidence_entry.code_target = method.function->code_target->entry;
    evidence_entry.generation = 1;
    evidence_entry.layout_status = dartplant::abi::DartCallLayoutStatus::kOk;
    evidence_entry.call_layout = layout;
    runtime.abi_evidence.push_back(std::move(evidence_entry));

    EXPECT_TRUE(dartplant::FindRuntimeCallLayoutLocked(&runtime, &method) == nullptr);
    method.function->code_target->identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
    EXPECT_EQ(layout.get(), dartplant::FindRuntimeCallLayoutLocked(&runtime, &method).get());
    runtime.generation->store(2, std::memory_order_release);
    EXPECT_TRUE(dartplant::FindRuntimeCallLayoutLocked(&runtime, &method) == nullptr);

    runtime.generation->store(1, std::memory_order_release);
    method.function->code_target->reported_alias_count = 2;
    EXPECT_TRUE(dartplant::FindRuntimeCallLayoutLocked(&runtime, &method) == nullptr);
}

TEST_CASE(InvocationReportsVerifiedAbiOverlayIndependentlyFromRawContext) {
    DartPlantInvocation invocation{};
    EXPECT_EQ(0, static_cast<int>(dartplant_invocation_has_verified_abi(&invocation)));

    dartplant::abi::DartCallLayout layout;
    layout.dart_sp_register = 15;
    invocation.call_layout = &layout;
    EXPECT_EQ(1, static_cast<int>(dartplant_invocation_has_verified_abi(&invocation)));
}

TEST_CASE(VerifiedClosureReceiverUsesHiddenArm64X0WithoutChangingFormalCount) {
    dartplant::abi::DartCallLayout layout;
    layout.dart_sp_register = 15;
    layout.has_closure_receiver = true;
    layout.closure_receiver_location = {
        .kind = dartplant::abi::DartAbiLocationKind::kGpRegister,
        .register_index = 0,
    };
    DartPlantArm64Context context{};
    context.x[0] = 0x501;
    DartPlantInvocation invocation{};
    invocation.call_layout = &layout;
    invocation.context = &context;
    invocation.phase = DARTPLANT_INVOCATION_ENTER;

    EXPECT_EQ(1U, static_cast<uint32_t>(dartplant_invocation_has_closure_receiver(&invocation)));
    EXPECT_EQ(0U, dartplant_invocation_argument_count(&invocation));
    DartPlantValue receiver{};
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_closure_receiver(&invocation, &receiver));
    EXPECT_EQ(DARTPLANT_VALUE_HEAP_OBJECT, receiver.kind);
    EXPECT_EQ(0x501ULL, receiver.raw);

    invocation.phase = DARTPLANT_INVOCATION_LEAVE;
    EXPECT_EQ(DARTPLANT_INVALID_INVOCATION_PHASE,
              dartplant_invocation_get_closure_receiver(&invocation, &receiver));
}

TEST_CASE(VerifiedPairOfTaggedResultUsesBothArm64ReturnRegisters) {
    constexpr uint64_t kCanonicalNull = 0x7400000001ULL;
    dartplant::abi::DartFunctionAbiResolution abi;
    abi.result = {
        .representation = dartplant::abi::DartAbiRepresentation::kPairOfTagged,
        .proof = dartplant::abi::DartAbiProofState::kProven,
    };
    abi.has_stack_calling_convention = true;
    abi.has_optional_parameter_info = true;
    abi.has_max_parameters_in_registers = true;
    abi.fully_proven = true;

    dartplant::abi::DartCallLayout layout;
    EXPECT_TRUE(dartplant::abi::ComputeArm64AotCallLayout(abi, &layout));
    EXPECT_EQ(2u, layout.result.location.count);

    DartPlantArm64Context context{};
    context.x[0] = kCanonicalNull;
    context.x[1] = 0x20;
    DartPlantInvocation invocation{};
    invocation.call_layout = &layout;
    invocation.context = &context;
    invocation.phase = DARTPLANT_INVOCATION_LEAVE;
    invocation.validated_null_value = kCanonicalNull;

    DartPlantValuePair pair{};
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_get_result_pair(&invocation, &pair));
    EXPECT_EQ(DARTPLANT_VALUE_NULL, pair.first.kind);
    EXPECT_EQ(DARTPLANT_VALUE_SMI, pair.second.kind);
    EXPECT_EQ(0x20ULL, pair.second.raw);

    const DartPlantValuePair replacement = {
        .first = {DARTPLANT_VALUE_SMI, 0, 0x40},
        .second = {DARTPLANT_VALUE_NULL, 0, 0},
    };
    EXPECT_EQ(DARTPLANT_OK, dartplant_invocation_set_result_pair(&invocation, &replacement));
    EXPECT_EQ(0x40ULL, context.x[0]);
    EXPECT_EQ(kCanonicalNull, context.x[1]);

    DartPlantValue single{};
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI, dartplant_invocation_get_result(&invocation, &single));
}

TEST_CASE(LeaveDispatcherCapturesPairAndFloatingPointReturnChannels) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);

    DartPlantMethod method{};
    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = std::make_shared<dartplant::DartCodeTarget>();
    hook.code_target->id = reinterpret_cast<uintptr_t>(Replacement);
    hook.code_target->entry = reinterpret_cast<uintptr_t>(Replacement);
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.profile = profile;
    hook.backup = reinterpret_cast<void*>(Replacement);

    DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = nullptr,
        .on_leave = CaptureWideReturnRegisters,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    DartPlantListener* listener = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant::AddCallbackListener(&hook, &method, options, 0, &listener));

    DartPlantArm64Context context{};
    EXPECT_TRUE(dartplant_arm64_dispatch_enter(&context, &hook).original != nullptr);
    constexpr uint64_t kPairSecond = 0x1122334455667788ULL;
    constexpr uint64_t kFpResult = 0x400921fb54442d18ULL;
    g_leave_x1 = 0;
    g_leave_v0 = 0;
    dartplant_arm64_dispatch_leave_from_tls(7, kPairSecond, kFpResult);
    EXPECT_EQ(kPairSecond, g_leave_x1);
    EXPECT_EQ(kFpResult, g_leave_v0);

    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(listener));
    dartplant_release_listener(listener);
}

TEST_CASE(InvocationDispatchRunsEnterLeaveAndOriginalPolicy) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT;
    profile.argument_count = 2;
    profile.argument_locations[0] = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
    profile.argument_locations[1] = {DARTPLANT_ABI_GP_REGISTER, 1, {0, 0}};
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

    DartPlantMethod method{};
    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = std::make_shared<dartplant::DartCodeTarget>();
    hook.code_target->id = reinterpret_cast<uintptr_t>(Replacement);
    hook.code_target->entry = reinterpret_cast<uintptr_t>(Replacement);
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.profile = profile;
    hook.backup = reinterpret_cast<void*>(Replacement);
    DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = OnEnter,
        .on_leave = OnLeave,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    DartPlantListener* listener = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant::AddCallbackListener(&hook, &method, options, 0, &listener));

    g_enter_calls = 0;
    g_leave_calls = 0;
    g_enter_depth = 0;
    g_leave_depth = 0;
    g_last_invocation = nullptr;
    DartPlantArm64Context context{};
    context.x[0] = 2;
    context.x[1] = 3;
    const auto enter = dartplant_arm64_dispatch_enter(&context, &hook);
    EXPECT_EQ(reinterpret_cast<void*>(Replacement), enter.original);
    EXPECT_TRUE(enter.context != nullptr);
    EXPECT_EQ(1, g_enter_calls);
    EXPECT_EQ(12, static_cast<int>(enter.context->x[0]));
    EXPECT_EQ(1U, g_enter_depth);

    const auto leave = dartplant_arm64_dispatch_leave_from_tls(15, 0, 0);
    EXPECT_EQ(1, g_leave_calls);
    EXPECT_EQ(1U, g_leave_depth);
    EXPECT_EQ(115, static_cast<int>(leave.result));
    EXPECT_EQ(115, static_cast<int>(leave.context->x[0]));

    listener->record->options.on_enter = SkipWithResult;
    listener->record->options.on_leave = nullptr;
    context.x[0] = 1;
    const auto skipped = dartplant_arm64_dispatch_enter(&context, &hook);
    EXPECT_EQ(nullptr, skipped.original);
    EXPECT_TRUE(skipped.context != nullptr);
    EXPECT_EQ(77, static_cast<int>(skipped.context->x[0]));

    listener->record->options.on_enter = CallOriginal;
    g_callback_status = DARTPLANT_OK;
    const auto explicit_original = dartplant_arm64_dispatch_enter(&context, &hook);
    // Host builds have no ARM64 synchronous-original bridge. A failed explicit
    // call must leave the invocation eligible for the normal automatic
    // original path instead of falsely marking it as already executed.
    EXPECT_EQ(DARTPLANT_HOOK_FAILED, g_callback_status);
    EXPECT_EQ(reinterpret_cast<void*>(Replacement), explicit_original.original);
    EXPECT_TRUE(g_last_invocation != nullptr);
    EXPECT_FALSE(dartplant_invocation_is_original_skipped(g_last_invocation));
    (void) dartplant_arm64_dispatch_leave_from_tls(15, 0, 0);
    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(listener));
    dartplant_release_listener(listener);
}

TEST_CASE(ReturnDispatchRequiresMatchingDartCallerLr) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT;
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

    DartPlantMethod method{};
    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = std::make_shared<dartplant::DartCodeTarget>();
    hook.code_target->entry = reinterpret_cast<uintptr_t>(Replacement);
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.profile = profile;
    hook.backup = reinterpret_cast<void*>(Replacement);
    const DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = nullptr,
        .on_leave = OnLeave,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    DartPlantListener* listener = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant::AddCallbackListener(&hook, &method, options, 0, &listener));

    g_leave_calls = 0;
    DartPlantArm64Context context{};
    context.x[30] = 0x12345678U;
    EXPECT_TRUE(dartplant_arm64_dispatch_enter(&context, &hook).original != nullptr);

    // A sibling entry can hit a RET patched to the same hook while the outer
    // invocation is still on TLS. Its LR identifies a different callsite, so
    // it must pass through without consuming the outer DispatchFrame.
    const auto sibling_return =
        dartplant_arm64_dispatch_return_from_hook(&hook, 7, 0, 0, 0x87654321U);
    EXPECT_TRUE(sibling_return.context == nullptr);
    EXPECT_EQ(0, g_leave_calls);

    const auto owned_return =
        dartplant_arm64_dispatch_return_from_hook(&hook, 9, 0, 0, context.x[30]);
    EXPECT_TRUE(owned_return.context != nullptr);
    EXPECT_EQ(1, g_leave_calls);

    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(listener));
    dartplant_release_listener(listener);
}

TEST_CASE(RuntimeBoundCallbackHookFailsClosedAfterGenerationAdvance) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT;
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

    DartPlantMethod method{};
    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = std::make_shared<dartplant::DartCodeTarget>();
    hook.code_target->entry = reinterpret_cast<uintptr_t>(Replacement);
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.profile = profile;
    hook.backup = reinterpret_cast<void*>(Replacement);
    hook.runtime_generation = std::make_shared<std::atomic_uint64_t>(7);
    hook.expected_runtime_generation = 7;

    const DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = OnEnter,
        .on_leave = nullptr,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    DartPlantListener* listener = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant::AddCallbackListener(&hook, &method, options, 0, &listener,
                                                           hook.runtime_generation, 7));

    hook.runtime_generation->store(8, std::memory_order_release);
    g_enter_calls = 0;
    DartPlantArm64Context context{};
    const auto stale = dartplant_arm64_dispatch_enter(&context, &hook);
    EXPECT_EQ(reinterpret_cast<void*>(Replacement), stale.original);
    EXPECT_EQ(0, g_enter_calls);

    DartPlantListener* rejected = nullptr;
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY,
              dartplant::AddCallbackListener(&hook, &method, options, 0, &rejected,
                                             hook.runtime_generation, 8));
    EXPECT_TRUE(rejected == nullptr);
    hook.state = dartplant::HookRecordState::kUnhooked;
    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(listener));
    dartplant_release_listener(listener);
}

TEST_CASE(HookChainUsesPrioritySnapshotAndPairedLeaveOrder) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT;
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

    DartPlantMethod method{};
    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = std::make_shared<dartplant::DartCodeTarget>();
    hook.code_target->id = reinterpret_cast<uintptr_t>(Replacement);
    hook.code_target->entry = reinterpret_cast<uintptr_t>(Replacement);
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.profile = profile;
    hook.backup = reinterpret_cast<void*>(Replacement);

    int high = 1;
    int low = 2;
    DartPlantHookOptions high_options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = OrderedEnter,
        .on_leave = OrderedLeave,
        .user_data = &high,
        .vm_adapter = nullptr,
    };
    DartPlantHookOptions low_options = high_options;
    low_options.user_data = &low;
    DartPlantListener* high_listener = nullptr;
    DartPlantListener* low_listener = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::AddCallbackListener(&hook, &method, low_options, 0, &low_listener));
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::AddCallbackListener(&hook, &method, high_options, 100, &high_listener));

    g_callback_order.clear();
    DartPlantArm64Context context{};
    const auto enter = dartplant_arm64_dispatch_enter(&context, &hook);
    EXPECT_EQ(reinterpret_cast<void*>(Replacement), enter.original);
    EXPECT_EQ(2U, g_callback_order.size());
    EXPECT_EQ(1, g_callback_order[0]);
    EXPECT_EQ(2, g_callback_order[1]);

    // Removal only affects future snapshots. This invocation still gets the
    // corresponding low-priority leave callback.
    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(low_listener));
    EXPECT_FALSE(dartplant_listener_is_idle(low_listener));
    dartplant_arm64_dispatch_leave_from_tls(9, 0, 0);
    EXPECT_EQ(4U, g_callback_order.size());
    EXPECT_EQ(-2, g_callback_order[2]);
    EXPECT_EQ(-1, g_callback_order[3]);
    EXPECT_TRUE(dartplant_listener_is_idle(low_listener));

    dartplant_release_listener(low_listener);
    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(high_listener));
    dartplant_release_listener(high_listener);
}

TEST_CASE(LateSharedCodeWithoutOptInBypassesCallbacksAndVerifiedAbi) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);

    auto target = std::make_shared<dartplant::DartCodeTarget>();
    target->id = reinterpret_cast<uintptr_t>(Replacement);
    target->entry = reinterpret_cast<uintptr_t>(Replacement);
    target->identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
    const dartplant::DartMethodIdentity first_identity = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "first",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    target->AddAlias(first_identity);

    DartPlantMethod method{};
    method.function = std::make_shared<dartplant::DartFunctionHandle>();
    method.function->identity = first_identity;
    method.function->source = dartplant::DartFunctionSource::kSynthetic;
    method.function->code_target = target;

    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = target;
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.profile = profile;
    hook.backup = reinterpret_cast<void*>(Replacement);
    hook.shared_code_opt_in = false;
    auto layout = std::make_shared<dartplant::abi::DartCallLayout>();
    layout->dart_sp_register = 15;
    hook.call_layout = layout;

    DartPlantHookOptions options = {
        .struct_size = sizeof(options),
        .flags = 0,
        .on_enter = LateSharedEnter,
        .on_leave = LateSharedLeave,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    DartPlantListener* listener = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant::AddCallbackListener(&hook, &method, options, 0, &listener));

    const dartplant::DartMethodIdentity late_alias = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "second",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    target->AddAlias(late_alias);
    EXPECT_TRUE(target->IsShared());

    g_late_shared_enter_calls = 0;
    g_late_shared_leave_calls = 0;
    DartPlantArm64Context context{};
    const auto enter = dartplant_arm64_dispatch_enter(&context, &hook);
    EXPECT_EQ(reinterpret_cast<void*>(Replacement), enter.original);
    EXPECT_EQ(0, g_late_shared_enter_calls);
    EXPECT_EQ(0, g_late_shared_leave_calls);
    dartplant_arm64_dispatch_leave_from_tls(9, 0, 0);
    EXPECT_EQ(0, g_late_shared_enter_calls);
    EXPECT_EQ(0, g_late_shared_leave_calls);
    EXPECT_EQ(0ULL, hook.in_flight);
    EXPECT_TRUE(dartplant_listener_is_idle(listener));

    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(listener));
    dartplant_release_listener(listener);
}

TEST_CASE(LateSharedCodeWithOptInKeepsRawCallbackButDropsVerifiedAbi) {
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);

    auto target = std::make_shared<dartplant::DartCodeTarget>();
    target->id = reinterpret_cast<uintptr_t>(Replacement);
    target->entry = reinterpret_cast<uintptr_t>(Replacement);
    target->identity_proof = DARTPLANT_CODE_IDENTITY_UNIQUE;
    const dartplant::DartMethodIdentity first_identity = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "first",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    target->AddAlias(first_identity);

    DartPlantMethod method{};
    method.function = std::make_shared<dartplant::DartFunctionHandle>();
    method.function->identity = first_identity;
    method.function->source = dartplant::DartFunctionSource::kSynthetic;
    method.function->code_target = target;

    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = target;
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.profile = profile;
    hook.backup = reinterpret_cast<void*>(Replacement);
    hook.shared_code_opt_in = true;
    auto layout = std::make_shared<dartplant::abi::DartCallLayout>();
    layout->dart_sp_register = 15;
    hook.call_layout = layout;

    DartPlantHookOptions options = {
        .struct_size = sizeof(options),
        .flags = DARTPLANT_HOOK_ALLOW_SHARED_CODE,
        .on_enter = LateSharedEnter,
        .on_leave = LateSharedLeave,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    DartPlantListener* listener = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant::AddCallbackListener(&hook, &method, options, 0, &listener));

    const dartplant::DartMethodIdentity late_alias = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "second",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    target->AddAlias(late_alias);

    g_late_shared_enter_calls = 0;
    g_late_shared_leave_calls = 0;
    g_late_shared_identity_ambiguous = 0;
    g_late_shared_verified_abi = 1;
    g_late_shared_raw_status = DARTPLANT_NOT_INITIALIZED;
    g_late_shared_typed_status = DARTPLANT_NOT_INITIALIZED;
    DartPlantArm64Context context{};
    context.x[1] = 0x44;
    const auto enter = dartplant_arm64_dispatch_enter(&context, &hook);
    EXPECT_EQ(reinterpret_cast<void*>(Replacement), enter.original);
    EXPECT_EQ(1, g_late_shared_enter_calls);
    EXPECT_EQ(1, static_cast<int>(g_late_shared_identity_ambiguous));
    EXPECT_EQ(0, static_cast<int>(g_late_shared_verified_abi));
    EXPECT_EQ(DARTPLANT_OK, g_late_shared_raw_status);
    EXPECT_EQ(DARTPLANT_UNSUPPORTED_ABI, g_late_shared_typed_status);
    dartplant_arm64_dispatch_leave_from_tls(9, 0, 0);
    EXPECT_EQ(1, g_late_shared_leave_calls);

    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(listener));
    dartplant_release_listener(listener);
}

TEST_CASE(LiveVmProfilesRequireExactSnapshotIdentity) {
    struct ExpectedProfile {
        const char* hash;
        const char* version;
        uint32_t profile_version;
        uint32_t global_pool_offset;
        uint32_t libraries_offset;
        uint32_t object_pool_cid;
    };
    constexpr ExpectedProfile expected[] = {
        {"d20a1be77c3d3c41b2a5accaee1ce549", "3.4.4", 1, 0x758, 0x3a8, 22},
        {"80a49c7111088100a233b2ae788e1f48", "3.5.0", 2, 0x778, 0x380, 22},
        {"ace654289f5abc240509fc941453ebc5", "3.12.1", 3, 0x6e0, 0x3e0, 23},
    };
    for (const auto& item : expected) {
        DartPlantFlutterSnapshotInfo snapshot{};
        snapshot.struct_size = sizeof(snapshot);
        snapshot.snapshot_hash = item.hash;
        snapshot.profile_name = "flutter-arm64-product-compressed";
        snapshot.compressed_pointers = 1;
        DartPlantLiveVmProfile profile{};
        profile.struct_size = sizeof(profile);
        EXPECT_EQ(DARTPLANT_OK, dartplant_live_vm_select_profile(&snapshot, &profile));
        EXPECT_EQ(item.profile_version, profile.profile_version);
        EXPECT_EQ(std::string(item.version), std::string(profile.dart_version));
        EXPECT_EQ(item.global_pool_offset, profile.thread_global_object_pool_offset);
        EXPECT_EQ(item.libraries_offset, profile.object_store_libraries_offset);
        EXPECT_EQ(item.object_pool_cid, profile.cid_object_pool);
        EXPECT_EQ(0x10U, profile.object_pool_elements_offset);

        DartPlantLiveVmProfile legacy_prefix{};
        legacy_prefix.struct_size =
            offsetof(DartPlantLiveVmProfile, code_unchecked_entry_point_offset);
        legacy_prefix.code_unchecked_entry_point_offset = 0xfeedbeefU;
        EXPECT_EQ(DARTPLANT_OK, dartplant_live_vm_select_profile(&snapshot, &legacy_prefix));
        EXPECT_EQ(offsetof(DartPlantLiveVmProfile, code_unchecked_entry_point_offset),
                  legacy_prefix.struct_size);
        EXPECT_EQ(item.profile_version, legacy_prefix.profile_version);
        EXPECT_EQ(item.global_pool_offset, legacy_prefix.thread_global_object_pool_offset);
        EXPECT_EQ(0xfeedbeefU, legacy_prefix.code_unchecked_entry_point_offset);
    }

    DartPlantFlutterSnapshotInfo unknown{};
    unknown.struct_size = sizeof(unknown);
    unknown.snapshot_hash = "00000000000000000000000000000000";
    unknown.profile_name = "flutter-arm64-product-compressed";
    unknown.compressed_pointers = 1;
    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    EXPECT_EQ(DARTPLANT_PROFILE_MISMATCH, dartplant_live_vm_select_profile(&unknown, &profile));
}

TEST_CASE(ListenerRemovalInsideEnterDefersUnhookUntilLeave) {
    InstallTestHost(FakeHook, FakeUnhook);
    g_fake_unhook_calls = 0;

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT;
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

    DartPlantMethod method{};
    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = std::make_shared<dartplant::DartCodeTarget>();
    hook.code_target->id = reinterpret_cast<uintptr_t>(Replacement);
    hook.code_target->entry = reinterpret_cast<uintptr_t>(Replacement);
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.profile = profile;
    hook.backup = reinterpret_cast<void*>(Replacement);

    DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = RemoveSelfOnEnter,
        .on_leave = SelfRemovedLeave,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    g_self_remove_leave_calls = 0;
    g_self_removing_listener = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant::AddCallbackListener(&hook, &method, options, 0,
                                                           &g_self_removing_listener));

    DartPlantArm64Context context{};
    const auto enter = dartplant_arm64_dispatch_enter(&context, &hook);
    EXPECT_EQ(reinterpret_cast<void*>(Replacement), enter.original);
    EXPECT_EQ(0, g_fake_unhook_calls);
    EXPECT_FALSE(dartplant_listener_is_active(g_self_removing_listener));
    EXPECT_FALSE(dartplant_listener_is_idle(g_self_removing_listener));

    dartplant_arm64_dispatch_leave_from_tls(9, 0, 0);
    EXPECT_EQ(1, g_self_remove_leave_calls);
    EXPECT_EQ(1, g_fake_unhook_calls);
    EXPECT_TRUE(dartplant_listener_is_idle(g_self_removing_listener));
    dartplant_release_listener(g_self_removing_listener);
    g_self_removing_listener = nullptr;
}
