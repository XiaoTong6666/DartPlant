// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "core/internal.h"
#include "dartplant/adapters/dobby.h"
#include "dartplant/invocation.h"
#include "dartplant/runtime.h"
#include "dartplant/runtime_profile.h"
#include "runtime/runtime_internal.h"

namespace {

using Add = int (*)(int, int);

Add g_original_add = nullptr;
int g_enter_calls = 0;
int g_leave_calls = 0;
int g_listener_calls = 0;
bool g_null_semantic_callback_ok = false;
bool g_raw_context_callback_ok = false;
bool g_dart_pad_branch_ok = false;

void OnEnter(DartPlantInvocation* invocation, void*) {
    ++g_enter_calls;
    DartPlantValue value{};
    if (dartplant_invocation_get_argument(invocation, 0, &value) == DARTPLANT_OK) {
        value.raw += 10;
        dartplant_invocation_set_argument(invocation, 0, &value);
    }
}

void OnLeave(DartPlantInvocation* invocation, void*) {
    ++g_leave_calls;
    DartPlantValue value{};
    if (dartplant_invocation_get_result(invocation, &value) == DARTPLANT_OK) {
        value.raw += 100;
        dartplant_invocation_set_result(invocation, &value);
    }
}

void SkipWithResult(DartPlantInvocation* invocation, void*) {
    const DartPlantValue value = {DARTPLANT_VALUE_RAW_WORD, 0, 77};
    dartplant_invocation_set_result(invocation, &value);
}

void SkipWithValidatedNull(DartPlantInvocation* invocation, void*) {
    const DartPlantValue null_value = {DARTPLANT_VALUE_NULL, 0, 0};
    DartPlantValue argument{};
    const DartPlantStatus set_argument =
        dartplant_invocation_set_argument(invocation, 0, &null_value);
    const DartPlantStatus get_argument =
        dartplant_invocation_get_argument(invocation, 0, &argument);
    const DartPlantStatus set_result = dartplant_invocation_set_result(invocation, &null_value);
    g_null_semantic_callback_ok = set_argument == DARTPLANT_OK && get_argument == DARTPLANT_OK &&
                                  argument.kind == DARTPLANT_VALUE_NULL &&
                                  set_result == DARTPLANT_OK;
}

void ObserveEnter(DartPlantInvocation*, void*) { ++g_listener_calls; }

void VerifyDartPadBranch(DartPlantInvocation* invocation, void*) {
    uint64_t spreg = 0;
    uintptr_t csp = 0;
#if defined(__aarch64__)
    asm volatile("mov %0, sp" : "=r"(csp));
#endif
    uint64_t argument = 0;
    g_dart_pad_branch_ok =
        dartplant_invocation_get_gp_register(invocation, 15, &spreg) == DARTPLANT_OK &&
        dartplant_invocation_get_gp_register(invocation, 0, &argument) == DARTPLANT_OK &&
        (spreg & 0xfU) == 8 && (csp & 0xfU) == 0 && argument == 5;
    if (dartplant_invocation_set_gp_register(invocation, 0, 77) != DARTPLANT_OK ||
        dartplant_invocation_skip_original(invocation) != DARTPLANT_OK) {
        g_dart_pad_branch_ok = false;
    }
}

extern "C" uint64_t DartPlantDeviceInvokeDartCallbackWithOddSp(DartPlantHook* hook,
                                                               uintptr_t dart_spreg,
                                                               uint64_t argument);
extern "C" int DartPlantFixtureAdd(int left, int right);

int Fail(const char* message);

int ExerciseDartPadBranch() {
    constexpr size_t kFakeStackSize = 1U << 20;
    void* mapping =
        mmap(nullptr, kFakeStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return Fail("map deterministic Dart pad stack");

    const uintptr_t end = reinterpret_cast<uintptr_t>(mapping) + kFakeStackSize;
    const uintptr_t dart_spreg = ((end - (64U << 10)) & ~uintptr_t{0xf}) + 8;

    auto target = std::make_shared<dartplant::DartCodeTarget>();
    target->id = reinterpret_cast<uintptr_t>(DartPlantFixtureAdd);
    target->entry = reinterpret_cast<uintptr_t>(DartPlantFixtureAdd);
    target->code_size = 4;
    auto function = std::make_shared<dartplant::DartFunctionHandle>();
    function->identity = {
        .library_uri = "package:fixture/pad.dart",
        .class_name = "Fixture",
        .function_name = "padProbe",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    function->source = dartplant::DartFunctionSource::kSynthetic;
    function->code_target = target;
    target->AddAlias(function->identity);

    DartPlantMethod method{};
    method.record.function_name = "padProbe";
    method.function = function;
    DartPlantHook hook{};
    hook.active = true;
    hook.has_method = true;
    hook.state = dartplant::HookRecordState::kInstalled;
    hook.code_target = target;
    hook.method_storage = std::make_unique<DartPlantMethod>(method);
    hook.backup = reinterpret_cast<void*>(DartPlantFixtureAdd);
    dartplant_runtime_profile_init_arm64_aot(&hook.profile);

    const DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = VerifyDartPadBranch,
        .on_leave = nullptr,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    DartPlantListener* listener = nullptr;
    const DartPlantStatus add_status =
        dartplant::AddCallbackListener(&hook, &method, options, 0, &listener);
    g_dart_pad_branch_ok = false;
    const uint64_t result = add_status == DARTPLANT_OK
                                ? DartPlantDeviceInvokeDartCallbackWithOddSp(&hook, dart_spreg, 5)
                                : 0;
    const bool idle = listener != nullptr && dartplant_listener_is_idle(listener) != 0;
    if (listener != nullptr) {
        (void) dartplant_remove_listener(listener);
        dartplant_release_listener(listener);
    }
    munmap(mapping, kFakeStackSize);
    if (add_status != DARTPLANT_OK || result != 77 || !g_dart_pad_branch_ok || !idle) {
        return Fail("deterministic Dart x15 alignment pad");
    }
    return 0;
}

void ObserveRawContext(DartPlantInvocation* invocation, void*) {
    uint64_t x0 = 0;
    DartPlantValue typed{};
    g_raw_context_callback_ok =
        dartplant_invocation_has_verified_abi(invocation) == 0 &&
        dartplant_invocation_get_gp_register(invocation, 0, &x0) == DARTPLANT_OK && x0 == 2 &&
        dartplant_invocation_get_argument(invocation, 0, &typed) == DARTPLANT_UNSUPPORTED_ABI;
}

__attribute__((noinline)) int HookedAdd(int left, int right) {
    return g_original_add(left, right) + 100;
}

int Fail(const char* message) {
    std::fprintf(stderr, "[FAIL] %s: %s\n", message, dartplant_last_error());
    return 1;
}

int ExerciseListenerChain(const DartPlantMethod* method,
                          const DartPlantHookOptions& callback_options) {
    DartPlantHookOptions listener_options = callback_options;
    listener_options.on_enter = ObserveEnter;
    listener_options.on_leave = nullptr;
    DartPlantListener* listener = nullptr;
    if (dartplant::AddCallbackListenerForMethod(method, listener_options, 100, &listener) !=
        DARTPLANT_OK) {
        return Fail("public listener chain add");
    }
    if (!dartplant_listener_is_active(listener)) {
        return Fail("public listener chain active");
    }
    if (DartPlantFixtureAdd(2, 3) != 115) {
        return Fail("public listener chain call");
    }
    if (g_listener_calls != 1) {
        return Fail("public listener chain");
    }
    if (dartplant_remove_listener(listener) != DARTPLANT_OK ||
        dartplant_listener_is_active(listener) || !dartplant_listener_is_idle(listener)) {
        return Fail("listener removal and idle state");
    }
    dartplant_release_listener(listener);
    return 0;
}

}  // namespace

int main() {
#if !defined(__aarch64__)
    std::fprintf(stderr, "[FAIL] device test requires ARM64\n");
    return 1;
#endif

    if (dartplant_install_host_api(dartplant_dobby_host_api()) != DARTPLANT_OK) {
        return Fail("generic host API install");
    }
    if (ExerciseDartPadBranch() != 0) return 1;
    dartplant::RefreshModules();

    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(DartPlantFixtureAdd), &info) == 0 ||
        info.dli_fname == nullptr) {
        return Fail("dladdr fixture");
    }
    const std::string path(info.dli_fname);
    const size_t slash = path.find_last_of('/');
    const std::string module_name = slash == std::string::npos ? path : path.substr(slash + 1);

    Dl_info runtime_info{};
    if (dladdr(reinterpret_cast<void*>(dartplant_last_error), &runtime_info) == 0 ||
        runtime_info.dli_fname == nullptr) {
        return Fail("dladdr runtime");
    }
    const std::string runtime_path(runtime_info.dli_fname);
    const size_t runtime_slash = runtime_path.find_last_of('/');
    const std::string runtime_module_name =
        runtime_slash == std::string::npos ? runtime_path : runtime_path.substr(runtime_slash + 1);

    DartPlantAddressQuery query = {
        .struct_size = sizeof(query),
        .module_name = module_name.c_str(),
        .address = reinterpret_cast<uint64_t>(DartPlantFixtureAdd),
        .address_kind = DARTPLANT_ADDRESS_RUNTIME,
        .code_size = 4,
        .expected_build_id = nullptr,
        .expected_fingerprint = nullptr,
    };
    DartPlantHook* hook = nullptr;
    void* backup = nullptr;
    if (dartplant_hook_address(&query, reinterpret_cast<void*>(HookedAdd), &backup, &hook) !=
        DARTPLANT_OK) {
        return Fail("install address hook");
    }
    g_original_add = reinterpret_cast<Add>(backup);
    if (g_original_add == nullptr || DartPlantFixtureAdd(2, 3) != 105) {
        return Fail("replacement and original call");
    }

    DartPlantHook* duplicate = nullptr;
    void* duplicate_backup = nullptr;
    if (dartplant_hook_address(&query, reinterpret_cast<void*>(HookedAdd), &duplicate_backup,
                               &duplicate) != DARTPLANT_ALREADY_HOOKED) {
        return Fail("duplicate hook rejection");
    }

    if (dartplant_unhook(hook) != DARTPLANT_OK || DartPlantFixtureAdd(2, 3) != 5) {
        return Fail("unhook and restore");
    }
    dartplant_release_hook(hook);

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT;
    profile.argument_count = 2;
    profile.argument_locations[0] = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
    profile.argument_locations[1] = {DARTPLANT_ABI_GP_REGISTER, 1, {0, 0}};
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

    // This executable is intentionally not a Dart VM. Build a synthetic method
    // only to validate the ARM64 callback/trampoline backend; runtime Function
    // discovery is tested by the real Flutter fixture below this test layer.
    dartplant::DartCodeTargetRegistry code_targets;
    auto code_target =
        code_targets.GetOrCreate(reinterpret_cast<uintptr_t>(DartPlantFixtureAdd), 4, 0, 1);
    auto function = std::make_shared<dartplant::DartFunctionHandle>();
    function->identity = {
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "add",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    function->source = dartplant::DartFunctionSource::kSynthetic;
    function->code_target = code_target;
    code_target->AddAlias(function->identity);
    DartPlantMethod method{};
    method.record.library_uri = function->identity.library_uri;
    method.record.class_name = function->identity.class_name;
    method.record.function_name = function->identity.function_name;
    method.record.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    method.record.address_kind = DARTPLANT_ADDRESS_RUNTIME;
    method.record.address = reinterpret_cast<uintptr_t>(DartPlantFixtureAdd);
    method.record.code_size = 4;
    method.function = function;

    // A conservative profile can still install a raw callback. No argument
    // mapping is guessed: only the raw ARM64 context API is available.
    DartPlantRuntimeProfile raw_context_profile{};
    dartplant_runtime_profile_init_arm64_aot(&raw_context_profile);
    DartPlantHookOptions callback_options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = ObserveRawContext,
        .on_leave = nullptr,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    g_raw_context_callback_ok = false;
    DartPlantHook* callback_hook = nullptr;
    if (dartplant::InstallCallbackHook(&method, raw_context_profile, callback_options, 0,
                                       &callback_hook, nullptr) != DARTPLANT_OK ||
        DartPlantFixtureAdd(2, 3) != 5 || !g_raw_context_callback_ok) {
        return Fail("raw context callback without ABI mapping");
    }
    if (dartplant_unhook(callback_hook) != DARTPLANT_OK) {
        return Fail("raw context callback unhook");
    }
    dartplant_release_hook(callback_hook);

    callback_options.on_enter = OnEnter;
    callback_options.on_leave = OnLeave;
    g_enter_calls = 0;
    g_leave_calls = 0;
    g_listener_calls = 0;
    callback_hook = nullptr;
    if (dartplant::InstallCallbackHook(&method, profile, callback_options, 0, &callback_hook,
                                       nullptr) != DARTPLANT_OK) {
        return Fail("callback hook install");
    }
    if (g_listener_calls != 0 || DartPlantFixtureAdd(2, 3) != 115 || g_enter_calls != 1 ||
        g_leave_calls != 1) {
        return Fail("callback enter/leave hook");
    }
    if (ExerciseListenerChain(&method, callback_options) != 0) return 1;
    if (dartplant_unhook(callback_hook) != DARTPLANT_OK) {
        return Fail("callback unhook");
    }
    dartplant_release_hook(callback_hook);

    constexpr uint64_t kCanonicalNull = 0x100000001;
    DartPlantRuntimeProfile null_profile = profile;
    null_profile.flags |=
        DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS | DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    callback_options.on_enter = SkipWithValidatedNull;
    callback_options.on_leave = nullptr;
    callback_hook = nullptr;
    g_null_semantic_callback_ok = false;
    if (dartplant::InstallCallbackHook(&method, null_profile, callback_options, 0, &callback_hook,
                                       nullptr, kCanonicalNull) != DARTPLANT_OK ||
        DartPlantFixtureAdd(2, 3) != 1 || !g_null_semantic_callback_ok) {
        return Fail("callback validated null semantic hook");
    }
    if (dartplant_unhook(callback_hook) != DARTPLANT_OK) {
        return Fail("validated null callback unhook");
    }
    dartplant_release_hook(callback_hook);

    callback_options.on_enter = SkipWithResult;
    callback_options.on_leave = nullptr;
    callback_hook = nullptr;
    if (dartplant::InstallCallbackHook(&method, profile, callback_options, 0, &callback_hook,
                                       nullptr) != DARTPLANT_OK ||
        DartPlantFixtureAdd(2, 3) != 77) {
        return Fail("callback skip-original hook");
    }
    if (dartplant_unhook(callback_hook) != DARTPLANT_OK) {
        return Fail("skip callback unhook");
    }
    dartplant_release_hook(callback_hook);

    query.address = reinterpret_cast<uint64_t>(&query);
    if (dartplant_hook_address(&query, reinterpret_cast<void*>(HookedAdd), &backup, &hook) !=
        DARTPLANT_ADDRESS_OUTSIDE_EXECUTABLE) {
        return Fail("non-executable address rejection");
    }

    std::printf("[PASS] ARM64 deterministic Dart x15 alignment pad\n");
    std::printf("[PASS] ARM64 Dobby hook/original/unhook\n");
    std::printf("[PASS] ARM64 callback enter/leave/result mutation\n");
    std::printf("[PASS] ARM64 raw callback without ABI mapping\n");
    std::printf("[PASS] ARM64 callback skip-original\n");
    std::printf("[PASS] ARM64 validated null callback semantic\n");
    std::printf("[PASS] duplicate hook rejection\n");
    std::printf("[PASS] executable range validation\n");
    return 0;
}
