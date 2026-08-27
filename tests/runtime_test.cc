// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "dartplant/runtime.h"

#include <dlfcn.h>

#include <string>
#include <thread>
#include <vector>

#include "core/internal.h"
#include "dartplant/invocation.h"
#include "dartplant/runtime_profile.h"
#include "dartplant/vm_adapter.h"
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

void SkipWithResult(DartPlantInvocation* invocation, void*) {
    g_last_invocation = invocation;
    const DartPlantValue value = {DARTPLANT_VALUE_RAW_WORD, 0, 77};
    dartplant_invocation_set_result(invocation, &value);
}

void CallOriginal(DartPlantInvocation* invocation, void*) {
    g_last_invocation = invocation;
    dartplant_invocation_call_original(invocation);
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
    first_function->code_target = target;
    auto second_function = std::make_shared<dartplant::DartFunctionHandle>();
    second_function->identity = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "addInt",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
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
    dartplant_arm64_dispatch_leave_from_tls(0);

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

TEST_CASE(FailedRuntimeHookInvalidationRetainsTargetOwnership) {
    const DartPlantNativeApiEntries failing_entries = {2, FakeHook, FakeFailUnhook};
    dartplant::InstallHostApi(&failing_entries);
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

    const DartPlantNativeApiEntries working_entries = {2, FakeHook, FakeUnhook};
    dartplant::InstallHostApi(&working_entries);
    EXPECT_EQ(DARTPLANT_UNHOOK_FAILED, dartplant_unhook(hook));
    EXPECT_EQ(2, g_fake_unhook_calls);
    g_fake_fail_unhook_enabled = false;
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(hook));
    dartplant_release_hook(hook);
}

TEST_CASE(HookUnhooksWithItsInstallingBackend) {
    const DartPlantNativeApiEntries backend_a = {2, FakeHook, BackendAUnhook};
    const DartPlantNativeApiEntries backend_b = {2, FakeHook, BackendBUnhook};
    dartplant::InstallHostApi(&backend_a);
    g_backend_a_unhook_calls = 0;
    g_backend_b_unhook_calls = 0;

    DartPlantHook* hook = nullptr;
    void* backup = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant::InstallHook(reinterpret_cast<uintptr_t>(Replacement),
                                     reinterpret_cast<void*>(Replacement), &backup, &hook));
    dartplant::InstallHostApi(&backend_b);
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(hook));
    EXPECT_EQ(1, g_backend_a_unhook_calls);
    EXPECT_EQ(0, g_backend_b_unhook_calls);
    dartplant_release_hook(hook);
}

TEST_CASE(RetiredRuntimeHookRetainsBackendOwnership) {
    const DartPlantNativeApiEntries entries = {2, FakeHook, FakeUnhook};
    dartplant::InstallHostApi(&entries);
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
    const DartPlantNativeApiEntries entries = {2, FakeHook, FakeUnhook};
    dartplant::InstallHostApi(&entries);
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
    const DartPlantNativeApiEntries entries = {2, FakeHook, FakeUnhook};
    dartplant::InstallHostApi(&entries);
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

    const DartPlantValue bool_value = {DARTPLANT_VALUE_BOOL, 0, kCanonicalNull};
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

    const auto leave = dartplant_arm64_dispatch_leave_from_tls(15);
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
    const auto explicit_original = dartplant_arm64_dispatch_enter(&context, &hook);
    EXPECT_EQ(nullptr, explicit_original.original);
    EXPECT_TRUE(g_last_invocation != nullptr);
    EXPECT_TRUE(dartplant_invocation_is_original_skipped(g_last_invocation));
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
    dartplant_arm64_dispatch_leave_from_tls(9);
    EXPECT_EQ(4U, g_callback_order.size());
    EXPECT_EQ(-2, g_callback_order[2]);
    EXPECT_EQ(-1, g_callback_order[3]);
    EXPECT_TRUE(dartplant_listener_is_idle(low_listener));

    dartplant_release_listener(low_listener);
    EXPECT_EQ(DARTPLANT_OK, dartplant_remove_listener(high_listener));
    dartplant_release_listener(high_listener);
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
    const DartPlantNativeApiEntries entries = {2, FakeHook, FakeUnhook};
    dartplant::InstallHostApi(&entries);
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

    dartplant_arm64_dispatch_leave_from_tls(9);
    EXPECT_EQ(1, g_self_remove_leave_calls);
    EXPECT_EQ(1, g_fake_unhook_calls);
    EXPECT_TRUE(dartplant_listener_is_idle(g_self_removing_listener));
    dartplant_release_listener(g_self_removing_listener);
    g_self_removing_listener = nullptr;
}
