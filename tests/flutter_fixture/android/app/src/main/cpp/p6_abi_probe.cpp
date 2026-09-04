// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "p6_abi_probe.h"

#include <android/log.h>
#include <unistd.h>

#include <atomic>
#include <bit>
#include <cstdint>

#include "dart_api_adapter.h"
#include "dartplant/dartplant.h"
#include "dartplant/hook.h"
#include "dartplant/invocation.h"
#include "fixture_host.h"

// These generated headers exist as empty placeholders during the first Flutter
// AOT build. The cold-bootstrap harness replaces them with exact libapp-bound
// ArtifactBundle registrars before rebuilding only the native fixture bridge.
#include "p6_entry_stack_sidecar.h"
#include "p6_forced_stack_sidecar.h"
#include "p6_int64_sidecar.h"
#include "p6_odd_stack_sidecar.h"
#include "p6_pair_sidecar.h"
#include "p6_throwing_stack_sidecar.h"

namespace {

constexpr char kTag[] = "DartPlantP6";

struct ProbeState {
    DartPlantHookHandle* int64_handle = nullptr;
    DartPlantHookHandle* entry_stack_handle = nullptr;
    DartPlantHookHandle* odd_stack_handle = nullptr;
    DartPlantHookHandle* throwing_stack_handle = nullptr;
    DartPlantHookHandle* forced_stack_handle = nullptr;
    DartPlantHookHandle* pair_handle = nullptr;
    DartPlantHookHandle* exception_lifetime_handle = nullptr;
    std::atomic_uint32_t int64_enter{0};
    std::atomic_uint32_t int64_leave{0};
    std::atomic_uint32_t entry_stack_enter{0};
    std::atomic_uint32_t entry_stack_leave{0};
    std::atomic_uint32_t odd_stack_enter{0};
    std::atomic_uint32_t odd_stack_leave{0};
    std::atomic_uint32_t throwing_stack_enter{0};
    std::atomic_uint32_t throwing_stack_leave{0};
    std::atomic_uint32_t throwing_stack_exception{0};
    std::atomic_uint32_t forced_stack_enter{0};
    std::atomic_uint32_t forced_stack_leave{0};
    std::atomic_uint32_t pair_leave{0};
    std::atomic_uint32_t moving_gc_pair_leave{0};
    std::atomic_uint32_t failures{0};
    std::atomic_uint32_t exception_lifetime_enter{0};
    std::atomic_uint32_t exception_lifetime_leave{0};
    std::atomic_uint32_t exception_object_observed{0};
    std::atomic_uint32_t exception_lifetime_failures{0};
    std::atomic_uint32_t exception_lifetime_unhook_ok{0};
};

ProbeState& State() {
    static ProbeState state;
    return state;
}

void Fail(const char* where) {
    State().failures.fetch_add(1, std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s: %s", where, dartplant_last_error());
}

bool RemoveHandle(DartPlantHookHandle** handle) {
    if (handle == nullptr || *handle == nullptr) return true;
    if (dartplant_unhook_handle(*handle) != DARTPLANT_OK ||
        dartplant_hook_handle_is_idle(*handle) == 0) {
        return false;
    }
    dartplant_release_hook_handle(*handle);
    *handle = nullptr;
    return true;
}

void Int64Enter(DartPlantInvocation* invocation, void*) {
    DartPlantValue left{};
    DartPlantValue right{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 2 ||
        dartplant_invocation_get_argument(invocation, 0, &left) != DARTPLANT_OK ||
        dartplant_invocation_get_argument(invocation, 1, &right) != DARTPLANT_OK ||
        left.kind != DARTPLANT_VALUE_INT64 || right.kind != DARTPLANT_VALUE_INT64) {
        Fail("int64 enter decode");
        return;
    }
    const int64_t left_value = std::bit_cast<int64_t>(left.raw);
    left.raw = std::bit_cast<uint64_t>(left_value + 1);
    if (dartplant_invocation_set_argument(invocation, 0, &left) != DARTPLANT_OK) {
        Fail("int64 enter rewrite");
        return;
    }
    // Exercise the synchronous native-callback -> Dart original -> native
    // callback transition on real PRODUCT AOT. The remaining P6 corpus hooks
    // still cover the normal automatic-original path.
    if (dartplant_invocation_call_original(invocation) != DARTPLANT_OK) {
        Fail("int64 call original");
        return;
    }
    State().int64_enter.fetch_add(1, std::memory_order_relaxed);
}

void Int64Leave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_INT64) {
        Fail("int64 leave decode");
        return;
    }
    const int64_t result_value = std::bit_cast<int64_t>(result.raw);
    result.raw = std::bit_cast<uint64_t>(result_value + 100);
    if (dartplant_invocation_set_result(invocation, &result) != DARTPLANT_OK) {
        Fail("int64 leave rewrite");
        return;
    }
    State().int64_leave.fetch_add(1, std::memory_order_relaxed);
}

void EntryStackEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue a6{};
    DartPlantValue a7{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 8 ||
        dartplant_invocation_get_argument(invocation, 6, &a6) != DARTPLANT_OK ||
        dartplant_invocation_get_argument(invocation, 7, &a7) != DARTPLANT_OK ||
        a6.kind != DARTPLANT_VALUE_DOUBLE || a7.kind != DARTPLANT_VALUE_DOUBLE) {
        Fail("entry stack decode");
        return;
    }
    const double a6_value = std::bit_cast<double>(a6.raw);
    const double a7_value = std::bit_cast<double>(a7.raw);
    a6.raw = std::bit_cast<uint64_t>(a6_value + 1.0);
    a7.raw = std::bit_cast<uint64_t>(a7_value + 2.0);
    if (dartplant_invocation_set_argument(invocation, 6, &a6) != DARTPLANT_OK ||
        dartplant_invocation_set_argument(invocation, 7, &a7) != DARTPLANT_OK) {
        Fail("entry stack rewrite");
        return;
    }
    State().entry_stack_enter.fetch_add(1, std::memory_order_relaxed);
}

void EntryStackLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_DOUBLE) {
        Fail("entry stack leave decode");
        return;
    }
    const double result_value = std::bit_cast<double>(result.raw);
    result.raw = std::bit_cast<uint64_t>(result_value + 1000.0);
    if (dartplant_invocation_set_result(invocation, &result) != DARTPLANT_OK) {
        Fail("entry stack leave rewrite");
        return;
    }
    State().entry_stack_leave.fetch_add(1, std::memory_order_relaxed);
}

void OddStackEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue a6{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 7 ||
        dartplant_invocation_get_argument(invocation, 6, &a6) != DARTPLANT_OK ||
        a6.kind != DARTPLANT_VALUE_DOUBLE) {
        Fail("odd stack decode");
        return;
    }
    const double a6_value = std::bit_cast<double>(a6.raw);
    a6.raw = std::bit_cast<uint64_t>(a6_value + 1.0);
    if (dartplant_invocation_set_argument(invocation, 6, &a6) != DARTPLANT_OK) {
        Fail("odd stack rewrite");
        return;
    }
    State().odd_stack_enter.fetch_add(1, std::memory_order_relaxed);
}

void OddStackLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_DOUBLE) {
        Fail("odd stack leave decode");
        return;
    }
    const double result_value = std::bit_cast<double>(result.raw);
    result.raw = std::bit_cast<uint64_t>(result_value + 100.0);
    if (dartplant_invocation_set_result(invocation, &result) != DARTPLANT_OK) {
        Fail("odd stack leave rewrite");
        return;
    }
    State().odd_stack_leave.fetch_add(1, std::memory_order_relaxed);
}

void ThrowingStackEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue first{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 8 ||
        dartplant_invocation_get_argument(invocation, 0, &first) != DARTPLANT_OK ||
        first.kind != DARTPLANT_VALUE_DOUBLE) {
        Fail("throwing stack enter decode");
        return;
    }
    State().throwing_stack_enter.fetch_add(1, std::memory_order_relaxed);
}

void ThrowingStackLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_DOUBLE) {
        Fail("throwing stack leave decode");
        return;
    }
    State().throwing_stack_leave.fetch_add(1, std::memory_order_relaxed);
}

void ThrowingStackException(DartPlantInvocation* invocation, void*) {
    if (dartplant_invocation_phase(invocation) != DARTPLANT_INVOCATION_EXCEPTION) {
        Fail("throwing exception phase");
        return;
    }
    DartPlantValue exception{};
    DartPlantValue stacktrace{};
    if (dartplant_invocation_get_exception(invocation, &exception) != DARTPLANT_OK ||
        dartplant_invocation_get_stacktrace(invocation, &stacktrace) != DARTPLANT_OK ||
        exception.kind != DARTPLANT_VALUE_HEAP_OBJECT ||
        stacktrace.kind != DARTPLANT_VALUE_HEAP_OBJECT) {
        Fail("throwing exception object observation");
        return;
    }
    State().exception_object_observed.fetch_add(1, std::memory_order_relaxed);
    State().throwing_stack_exception.fetch_add(1, std::memory_order_relaxed);
}

void ExceptionLifetimeEnter(DartPlantInvocation* invocation, void*) {
    auto& state = State();
    DartPlantValue first{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 8 ||
        dartplant_invocation_get_argument(invocation, 0, &first) != DARTPLANT_OK ||
        first.kind != DARTPLANT_VALUE_DOUBLE) {
        state.exception_lifetime_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    state.exception_lifetime_enter.fetch_add(1, std::memory_order_relaxed);
    if (std::bit_cast<double>(first.raw) != 99.0) return;

    // Deterministically exercise the old self-unhook race: this is the only
    // active real-Dart hook/exception-bridge consumer. Removing its listener
    // while this invocation is in flight moves the physical hook to
    // kUnhooking. The subsequent Dart throw makes JumpToFrame cleanup drop the
    // final in-flight reference while the process-global exception bridge is
    // itself executing.
    DartPlantHookHandle* handle = state.exception_lifetime_handle;
    if (handle == nullptr || dartplant_unhook_handle(handle) != DARTPLANT_OK) {
        state.exception_lifetime_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    state.exception_lifetime_unhook_ok.store(1, std::memory_order_release);
}

void ExceptionLifetimeLeave(DartPlantInvocation*, void*) {
    State().exception_lifetime_leave.fetch_add(1, std::memory_order_relaxed);
}

void ForcedStackEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue left{};
    DartPlantValue right{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 2 ||
        dartplant_invocation_get_argument(invocation, 0, &left) != DARTPLANT_OK ||
        dartplant_invocation_get_argument(invocation, 1, &right) != DARTPLANT_OK ||
        left.kind != DARTPLANT_VALUE_SMI || right.kind != DARTPLANT_VALUE_SMI) {
        Fail("forced stack decode");
        return;
    }
    if (dartplant_invocation_set_argument(invocation, 0, &right) != DARTPLANT_OK ||
        dartplant_invocation_set_argument(invocation, 1, &left) != DARTPLANT_OK) {
        Fail("forced stack swap");
        return;
    }
    State().forced_stack_enter.fetch_add(1, std::memory_order_relaxed);
}

void ForcedStackLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_SMI) {
        Fail("forced stack result");
        return;
    }
    State().forced_stack_leave.fetch_add(1, std::memory_order_relaxed);
}

void PairLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValuePair pair{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_get_result_pair(invocation, &pair) != DARTPLANT_OK) {
        Fail("pair decode");
        return;
    }
    const bool smi_pair =
        pair.first.kind == DARTPLANT_VALUE_SMI && pair.second.kind == DARTPLANT_VALUE_SMI;
    const bool object_pair = pair.first.kind == DARTPLANT_VALUE_HEAP_OBJECT &&
                             pair.second.kind == DARTPLANT_VALUE_HEAP_OBJECT;
    if (!smi_pair && !object_pair) {
        Fail("pair semantic kinds");
        return;
    }
    if (object_pair) {
        // A sibling isolate in the same isolate group allocates aggressively
        // while this mutator is native and at a safepoint.
        usleep(500000);
        if (dartplant_invocation_get_result_pair(invocation, &pair) != DARTPLANT_OK ||
            pair.first.kind != DARTPLANT_VALUE_HEAP_OBJECT ||
            pair.second.kind != DARTPLANT_VALUE_HEAP_OBJECT) {
            Fail("pair relocated root refresh");
            return;
        }
        State().moving_gc_pair_leave.fetch_add(1, std::memory_order_relaxed);
    }
    const DartPlantValuePair swapped = {
        .first = pair.second,
        .second = pair.first,
    };
    if (dartplant_invocation_set_result_pair(invocation, &swapped) != DARTPLANT_OK) {
        Fail("pair swap");
        return;
    }
    State().pair_leave.fetch_add(1, std::memory_order_relaxed);
}

DartPlantStatus InstallOne(const char* function_name, DartPlantInvocationCallback on_enter,
                           DartPlantInvocationCallback on_leave, DartPlantHookHandle** out_handle,
                           bool use_vm_adapter = false) {
    const DartPlantMethodQuery query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "Global",
        .function_name = function_name,
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    DartPlantStatus status = dartplant_find_method(&query, &method);
    if (status != DARTPLANT_OK) return status;
    const DartPlantHookOptions options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = 0,
        .on_enter = on_enter,
        .on_leave = on_leave,
        .user_data = nullptr,
        .vm_adapter = use_vm_adapter ? dartplant_fixture_dart_api_adapter() : nullptr,
    };
    status = dartplant_hook_method(method, &options, out_handle);
    dartplant_release_method(method);
    return status;
}

}  // namespace

extern "C" int32_t dartplant_fixture_p6_abi_install() {
    auto& state = State();
    if (state.int64_handle != nullptr || state.entry_stack_handle != nullptr ||
        state.odd_stack_handle != nullptr || state.throwing_stack_handle != nullptr ||
        state.forced_stack_handle != nullptr || state.pair_handle != nullptr) {
        return DARTPLANT_ALREADY_HOOKED;
    }
    state.int64_enter.store(0, std::memory_order_relaxed);
    state.int64_leave.store(0, std::memory_order_relaxed);
    state.entry_stack_enter.store(0, std::memory_order_relaxed);
    state.entry_stack_leave.store(0, std::memory_order_relaxed);
    state.odd_stack_enter.store(0, std::memory_order_relaxed);
    state.odd_stack_leave.store(0, std::memory_order_relaxed);
    state.throwing_stack_enter.store(0, std::memory_order_relaxed);
    state.throwing_stack_leave.store(0, std::memory_order_relaxed);
    state.throwing_stack_exception.store(0, std::memory_order_relaxed);
    state.exception_object_observed.store(0, std::memory_order_relaxed);
    state.forced_stack_enter.store(0, std::memory_order_relaxed);
    state.forced_stack_leave.store(0, std::memory_order_relaxed);
    state.pair_leave.store(0, std::memory_order_relaxed);
    state.moving_gc_pair_leave.store(0, std::memory_order_relaxed);
    state.failures.store(0, std::memory_order_relaxed);

    DartPlantStatus status = dartplant_fixture::InstallLocalGateHost();
    if (status != DARTPLANT_OK) return status;
    const DartPlantInitInfo init = {
        .struct_size = sizeof(DartPlantInitInfo),
        .version = DARTPLANT_INIT_API_VERSION,
        .host_api = nullptr,
        .artifact_bundle = nullptr,
        .app_module_name = nullptr,
        .runtime_module_name = nullptr,
    };
    status = dartplant_init(&init);
    if (status == DARTPLANT_OK) {
        status = InstallOne("verifiedAbiInt64", Int64Enter, Int64Leave, &state.int64_handle);
    }
    if (status == DARTPLANT_OK) {
        status = InstallOne("verifiedAbiEntryStack", EntryStackEnter, EntryStackLeave,
                            &state.entry_stack_handle);
    }
    if (status == DARTPLANT_OK) {
        status = InstallOne("verifiedAbiOddStack", OddStackEnter, OddStackLeave,
                            &state.odd_stack_handle);
    }
    if (status == DARTPLANT_OK) {
        status = InstallOne("verifiedAbiThrowingStack", ThrowingStackEnter, ThrowingStackLeave,
                            &state.throwing_stack_handle, true);
        if (status == DARTPLANT_OK) {
            status = dartplant_hook_handle_set_exception_callback(state.throwing_stack_handle,
                                                                  ThrowingStackException, nullptr);
        }
    }
    if (status == DARTPLANT_OK) {
        status = InstallOne("verifiedAbiForcedStack", ForcedStackEnter, ForcedStackLeave,
                            &state.forced_stack_handle);
    }
    if (status == DARTPLANT_OK) {
        status = InstallOne("verifiedAbiPair", nullptr, PairLeave, &state.pair_handle, true);
    }
    const bool ready = status == DARTPLANT_OK && state.int64_handle != nullptr &&
                       state.entry_stack_handle != nullptr && state.odd_stack_handle != nullptr &&
                       state.throwing_stack_handle != nullptr &&
                       state.forced_stack_handle != nullptr && state.pair_handle != nullptr;
    __android_log_print(ready ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                        "P6 ABI install ready=%u status=%d error=%s", static_cast<unsigned>(ready),
                        status, ready ? "none" : dartplant_last_error());
    if (!ready) {
        dartplant_fixture_p6_abi_cleanup();
        return status == DARTPLANT_OK ? DARTPLANT_HOOK_FAILED : status;
    }
    return DARTPLANT_OK;
}

extern "C" uint64_t dartplant_fixture_p6_abi_probe() {
    auto& state = State();
    const uint32_t int64_enter = state.int64_enter.load(std::memory_order_relaxed);
    const uint32_t int64_leave = state.int64_leave.load(std::memory_order_relaxed);
    const uint32_t entry_enter = state.entry_stack_enter.load(std::memory_order_relaxed);
    const uint32_t entry_leave = state.entry_stack_leave.load(std::memory_order_relaxed);
    const uint32_t odd_enter = state.odd_stack_enter.load(std::memory_order_relaxed);
    const uint32_t odd_leave = state.odd_stack_leave.load(std::memory_order_relaxed);
    const uint32_t throwing_enter = state.throwing_stack_enter.load(std::memory_order_relaxed);
    const uint32_t throwing_leave = state.throwing_stack_leave.load(std::memory_order_relaxed);
    const uint32_t throwing_exception =
        state.throwing_stack_exception.load(std::memory_order_relaxed);
    const uint32_t exception_object_observed =
        state.exception_object_observed.load(std::memory_order_relaxed);
    const uint32_t forced_enter = state.forced_stack_enter.load(std::memory_order_relaxed);
    const uint32_t forced_leave = state.forced_stack_leave.load(std::memory_order_relaxed);
    const uint32_t pair_leave = state.pair_leave.load(std::memory_order_relaxed);
    const uint32_t moving_gc_pair_leave =
        state.moving_gc_pair_leave.load(std::memory_order_relaxed);
    const uint32_t failures = state.failures.load(std::memory_order_relaxed);
    const bool int64_ok = int64_enter == 1 && int64_leave == 1;
    const bool entry_ok = entry_enter == 1 && entry_leave == 1;
    const bool odd_ok = odd_enter == 1 && odd_leave == 1;
    // The throwing call enters but must not run on_leave. JumpToFrame cleanup
    // retires that invocation, then the following normal call pairs enter/leave.
    const bool throw_ok = throwing_enter == 2 && throwing_leave == 1 && throwing_exception == 1 &&
                          exception_object_observed == 1;
    const bool forced_ok = forced_enter == 1 && forced_leave == 1;
    const bool pair_ok = pair_leave == 2 && moving_gc_pair_leave == 1;

    const bool cleanup =
        RemoveHandle(&state.pair_handle) && RemoveHandle(&state.forced_stack_handle) &&
        RemoveHandle(&state.throwing_stack_handle) && RemoveHandle(&state.odd_stack_handle) &&
        RemoveHandle(&state.entry_stack_handle) && RemoveHandle(&state.int64_handle);
    if (dartplant_is_initialized() != 0) dartplant_shutdown();
    const bool shutdown = dartplant_is_initialized() == 0;
    const bool passed = int64_ok && entry_ok && odd_ok && throw_ok && forced_ok && pair_ok &&
                        failures == 0 && cleanup && shutdown;
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
        "P6 ABI probe int64=%u entry_stack=%u odd_stack=%u throw=%u forced_stack=%u pair=%u failures=%u cleanup=%u shutdown=%u passed=%u",
        static_cast<unsigned>(int64_ok), static_cast<unsigned>(entry_ok),
        static_cast<unsigned>(odd_ok), static_cast<unsigned>(throw_ok),
        static_cast<unsigned>(forced_ok), static_cast<unsigned>(pair_ok), failures,
        static_cast<unsigned>(cleanup), static_cast<unsigned>(shutdown),
        static_cast<unsigned>(passed));
    return passed ? 1 : 0;
}

extern "C" void dartplant_fixture_p6_abi_cleanup() {
    auto& state = State();
    (void) RemoveHandle(&state.pair_handle);
    (void) RemoveHandle(&state.forced_stack_handle);
    (void) RemoveHandle(&state.throwing_stack_handle);
    (void) RemoveHandle(&state.odd_stack_handle);
    (void) RemoveHandle(&state.entry_stack_handle);
    (void) RemoveHandle(&state.int64_handle);
    if (dartplant_is_initialized() != 0) dartplant_shutdown();
}

extern "C" int32_t dartplant_fixture_exception_bridge_lifetime_install() {
    auto& state = State();
    if (state.exception_lifetime_handle != nullptr) return DARTPLANT_ALREADY_HOOKED;
    state.exception_lifetime_enter.store(0, std::memory_order_relaxed);
    state.exception_lifetime_leave.store(0, std::memory_order_relaxed);
    state.exception_lifetime_failures.store(0, std::memory_order_relaxed);
    state.exception_lifetime_unhook_ok.store(0, std::memory_order_relaxed);

    DartPlantStatus status = dartplant_fixture::InstallLocalGateHost();
    if (status != DARTPLANT_OK) return status;
    const DartPlantInitInfo init = {
        .struct_size = sizeof(DartPlantInitInfo),
        .version = DARTPLANT_INIT_API_VERSION,
        .host_api = nullptr,
        .artifact_bundle = nullptr,
        .app_module_name = nullptr,
        .runtime_module_name = nullptr,
    };
    status = dartplant_init(&init);
    if (status == DARTPLANT_OK) {
        status = InstallOne("verifiedAbiThrowingStack", ExceptionLifetimeEnter,
                            ExceptionLifetimeLeave, &state.exception_lifetime_handle, true);
    }
    const bool ready = status == DARTPLANT_OK && state.exception_lifetime_handle != nullptr;
    __android_log_print(ready ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                        "exception bridge lifetime install ready=%u status=%d error=%s",
                        static_cast<unsigned>(ready), status,
                        ready ? "none" : dartplant_last_error());
    if (!ready) {
        dartplant_fixture_exception_bridge_lifetime_cleanup();
        return status == DARTPLANT_OK ? DARTPLANT_HOOK_FAILED : status;
    }
    return DARTPLANT_OK;
}

extern "C" uint64_t dartplant_fixture_exception_bridge_lifetime_probe() {
    auto& state = State();
    const uint32_t enter = state.exception_lifetime_enter.load(std::memory_order_acquire);
    const uint32_t leave = state.exception_lifetime_leave.load(std::memory_order_acquire);
    const uint32_t failures = state.exception_lifetime_failures.load(std::memory_order_acquire);
    const bool unhook_requested =
        state.exception_lifetime_unhook_ok.load(std::memory_order_acquire) != 0;
    const bool idle = state.exception_lifetime_handle != nullptr &&
                      dartplant_hook_handle_is_idle(state.exception_lifetime_handle) != 0;
    const bool inactive = state.exception_lifetime_handle != nullptr &&
                          dartplant_hook_handle_is_active(state.exception_lifetime_handle) == 0;

    if (state.exception_lifetime_handle != nullptr && idle) {
        dartplant_release_hook_handle(state.exception_lifetime_handle);
        state.exception_lifetime_handle = nullptr;
    }
    if (dartplant_is_initialized() != 0) dartplant_shutdown();
    const bool shutdown = dartplant_is_initialized() == 0;
    const bool passed = enter == 1 && leave == 0 && failures == 0 && unhook_requested && idle &&
                        inactive && state.exception_lifetime_handle == nullptr && shutdown;
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
        "exception bridge lifetime probe enter=%u leave=%u unhook=%u idle=%u inactive=%u failures=%u shutdown=%u passed=%u",
        enter, leave, static_cast<unsigned>(unhook_requested), static_cast<unsigned>(idle),
        static_cast<unsigned>(inactive), failures, static_cast<unsigned>(shutdown),
        static_cast<unsigned>(passed));
    return passed ? 1 : 0;
}

extern "C" void dartplant_fixture_exception_bridge_lifetime_cleanup() {
    auto& state = State();
    if (state.exception_lifetime_handle != nullptr) {
        (void) dartplant_unhook_handle(state.exception_lifetime_handle);
        if (dartplant_hook_handle_is_idle(state.exception_lifetime_handle) != 0) {
            dartplant_release_hook_handle(state.exception_lifetime_handle);
            state.exception_lifetime_handle = nullptr;
        }
    }
    if (dartplant_is_initialized() != 0) dartplant_shutdown();
}
