#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "core/internal.h"
#include "dart_api_adapter.h"
#include "dartplant/invocation.h"
#include "dartplant/live_vm.h"
#include "dartplant/native_api.h"
#include "dartplant/runtime.h"
#include "dartplant/runtime_profile.h"
#include "standalone_dobby_host.h"

namespace {

constexpr char kTag[] = "DartPlantFixture";

DartPlantRuntime* g_runtime = nullptr;
DartPlantMethod* g_instrumented_add = nullptr;
DartPlantMethod* g_add_int = nullptr;
DartPlantMethod* g_echo_object = nullptr;
DartPlantMethod* g_negate_bool = nullptr;
DartPlantMethod* g_signature_probe = nullptr;
DartPlantHook* g_instrumented_add_hook = nullptr;
DartPlantHook* g_echo_object_hook = nullptr;
DartPlantHook* g_negate_bool_hook = nullptr;
DartPlantListener* g_add_int_listener = nullptr;
DartPlantFlutterSnapshotInfo g_snapshot_info{};
std::atomic_flag g_object_bridge_probe = ATOMIC_FLAG_INIT;
DartPlantObjectHandle* g_weak_object_handle = nullptr;
DartPlantObjectHandle* g_replacement_object_handle = nullptr;
std::atomic<uint32_t> g_object_callback_count = 0;
std::atomic<uint64_t> g_instrumented_add_enter = 0;
std::atomic<uint64_t> g_instrumented_add_leave = 0;
std::atomic<uint64_t> g_instrumented_add_last_result = 0;
std::atomic<uint64_t> g_live_vm_probe_ok = 0;
std::atomic<uint64_t> g_live_vm_probe_failed = 0;
std::atomic_bool g_runtime_live_vm_ready{false};
std::atomic_bool g_shared_policy_ok{false};
std::atomic_bool g_shared_identity_ambiguous_seen{false};
std::atomic<uint64_t> g_add_int_listener_enter = 0;
std::atomic_bool g_add_int_listener_identity_ok{false};
std::atomic<uint64_t> g_null_passthrough_count = 0;
std::atomic<uint64_t> g_null_result_overrides = 0;
std::atomic<uint64_t> g_null_result_count = 0;
std::atomic<uint64_t> g_null_semantic_failures = 0;
std::atomic<uint64_t> g_bool_true_results = 0;
std::atomic<uint64_t> g_bool_false_results = 0;
std::atomic<uint64_t> g_bool_semantic_failures = 0;
std::atomic<int32_t> g_cold_bootstrap_status{-1};
std::thread g_cold_bootstrap_thread;

void LogFailure(const char* operation) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s: %s", operation, dartplant_last_error());
}

[[maybe_unused]] void OnIntLeave(DartPlantInvocation* invocation, void*) { (void) invocation; }

[[maybe_unused]] void OnIntEnter(DartPlantInvocation* invocation, void*) {
    const DartPlantStatus status = dartplant_invocation_call_original(invocation);
    __android_log_print(ANDROID_LOG_INFO, kTag, "int invoke_original status=%d", status);
    DartPlantValue value{};
    if (status == DARTPLANT_OK &&
        dartplant_invocation_get_result(invocation, &value) == DARTPLANT_OK) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "int original result raw=0x%llx",
                            static_cast<unsigned long long>(value.raw));
    }
}

void OnInstrumentedAddEnter(DartPlantInvocation* invocation, void*) {
    const bool identity_ambiguous = dartplant_invocation_identity_ambiguous(invocation) != 0;
    const uint32_t invocation_alias_count = dartplant_invocation_code_alias_count(invocation);
    if (identity_ambiguous) {
        g_shared_identity_ambiguous_seen.store(true, std::memory_order_release);
    }
    DartPlantLiveVmProbeInfo live{};
    live.struct_size = sizeof(live);
    const DartPlantStatus live_status =
        dartplant_live_vm_probe_invocation(invocation, &g_snapshot_info, &live);
    const uintptr_t expected_entry =
        g_instrumented_add == nullptr ? 0 : dartplant_method_runtime_address(g_instrumented_add);
    bool live_ok = live_status == DARTPLANT_OK && live.heap_bits_match &&
                   live.null_register_match && live.thread_pool_match && live.code_pool_match &&
                   live.code_pool_is_null && live.code_owner_is_function &&
                   (live.function_code_match || live.code_owner_mismatch_allowed) &&
                   live.function_in_class_functions && live.owner_is_toplevel_class &&
                   live.function_found_from_vm_index && live.entry_alias_count != 0 &&
                   std::strcmp(live.function_name, "instrumentedAdd") == 0 &&
                   std::strcmp(live.class_name, "Global") == 0 &&
                   std::strcmp(live.library_uri, "package:dartplant_fixture/main.dart") == 0 &&
                   live.function_entry_point == expected_entry &&
                   (live.code_entry_matches_function || live.entry_is_shared);
    if (live_ok && !g_runtime_live_vm_ready.load(std::memory_order_acquire)) {
        const DartPlantStatus capture_status =
            dartplant_runtime_capture_live_vm(g_runtime, invocation);
        if (capture_status == DARTPLANT_OK) {
            g_runtime_live_vm_ready.store(true, std::memory_order_release);
        } else {
            live_ok = false;
            __android_log_print(ANDROID_LOG_ERROR, kTag, "runtime live-vm capture failed: %s",
                                dartplant_last_error());
        }
    }
    if (live_ok) {
        g_live_vm_probe_ok.fetch_add(1, std::memory_order_relaxed);
        __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "live-vm ok profile=%s thr=0x%llx group=0x%llx heap=0x%llx pp=0x%llx pool=0x%llx pool_len=%llu code_reg=0x%llx code=0x%llx code_owner=0x%llx code_entry=0x%llx fn_entry=0x%llx function=0x%llx code_pool_null=%u owner_match=%u dedup_owner=%u code_entry_match=%u aliases=%u shared=%u ambiguous=%u invocation_aliases=%u %s/%s/%s",
            live.profile_name, static_cast<unsigned long long>(live.thread),
            static_cast<unsigned long long>(live.isolate_group),
            static_cast<unsigned long long>(live.heap_base),
            static_cast<unsigned long long>(live.pp),
            static_cast<unsigned long long>(live.global_object_pool),
            static_cast<unsigned long long>(live.object_pool_length),
            static_cast<unsigned long long>(live.code_register),
            static_cast<unsigned long long>(live.code),
            static_cast<unsigned long long>(live.code_owner),
            static_cast<unsigned long long>(live.code_entry_point),
            static_cast<unsigned long long>(live.function_entry_point),
            static_cast<unsigned long long>(live.function),
            static_cast<unsigned>(live.code_pool_is_null),
            static_cast<unsigned>(live.function_code_match),
            static_cast<unsigned>(live.code_owner_mismatch_allowed),
            static_cast<unsigned>(live.code_entry_matches_function),
            static_cast<unsigned>(live.entry_alias_count),
            static_cast<unsigned>(live.entry_is_shared), static_cast<unsigned>(identity_ambiguous),
            invocation_alias_count, live.library_uri, live.class_name, live.function_name);
    } else {
        g_live_vm_probe_failed.fetch_add(1, std::memory_order_relaxed);
        uint64_t x22 = 0;
        uint64_t x24 = 0;
        uint64_t x26 = 0;
        uint64_t x27 = 0;
        uint64_t x28 = 0;
        (void) dartplant_invocation_get_gp_register(invocation, 22, &x22);
        (void) dartplant_invocation_get_gp_register(invocation, 24, &x24);
        (void) dartplant_invocation_get_gp_register(invocation, 26, &x26);
        (void) dartplant_invocation_get_gp_register(invocation, 27, &x27);
        (void) dartplant_invocation_get_gp_register(invocation, 28, &x28);
        __android_log_print(
            ANDROID_LOG_ERROR, kTag,
            "live-vm failed status=%d error=%s expected_entry=0x%llx x22=0x%llx x24=0x%llx x26=0x%llx x27=0x%llx x28=0x%llx actual_entry=0x%llx fn_entry=0x%llx code_owner=0x%llx aliases=%u shared=%u uri=%s class=%s fn=%s flags=%u%u%u%u%u%u%u%u%u%u%u%u",
            live_status, dartplant_last_error(), static_cast<unsigned long long>(expected_entry),
            static_cast<unsigned long long>(x22), static_cast<unsigned long long>(x24),
            static_cast<unsigned long long>(x26), static_cast<unsigned long long>(x27),
            static_cast<unsigned long long>(x28),
            static_cast<unsigned long long>(live.code_entry_point),
            static_cast<unsigned long long>(live.function_entry_point),
            static_cast<unsigned long long>(live.code_owner),
            static_cast<unsigned>(live.entry_alias_count),
            static_cast<unsigned>(live.entry_is_shared), live.library_uri, live.class_name,
            live.function_name, static_cast<unsigned>(live.heap_bits_match),
            static_cast<unsigned>(live.null_register_match),
            static_cast<unsigned>(live.thread_pool_match),
            static_cast<unsigned>(live.code_pool_match),
            static_cast<unsigned>(live.code_pool_is_null),
            static_cast<unsigned>(live.function_code_match),
            static_cast<unsigned>(live.code_owner_is_function),
            static_cast<unsigned>(live.code_owner_mismatch_allowed),
            static_cast<unsigned>(live.code_entry_matches_function),
            static_cast<unsigned>(live.function_in_class_functions),
            static_cast<unsigned>(live.owner_is_toplevel_class),
            static_cast<unsigned>(live.function_found_from_vm_index));
    }

    DartPlantValue left{};
    DartPlantValue right{};
    if (dartplant_invocation_get_argument(invocation, 0, &left) != DARTPLANT_OK ||
        dartplant_invocation_get_argument(invocation, 1, &right) != DARTPLANT_OK) {
        LogFailure("instrumentedAdd argument decode");
        return;
    }
    // Dart SMI values are tagged by shifting the signed integer left once.
    left.raw += 20;
    if (dartplant_invocation_set_argument(invocation, 0, &left) != DARTPLANT_OK) {
        LogFailure("instrumentedAdd argument rewrite");
        return;
    }
    g_instrumented_add_enter.fetch_add(1, std::memory_order_relaxed);
    (void) right;
}

void OnSharedAddIntListenerEnter(DartPlantInvocation* invocation, void*) {
    const DartPlantMethod* requested = dartplant_invocation_requested_method(invocation);
    const bool requested_ok =
        requested != nullptr &&
        requested->record.library_uri == "package:dartplant_fixture/main.dart" &&
        requested->record.class_name == "DartPlantFixture" &&
        requested->record.function_name == "addInt";
    const bool semantics_ok = requested_ok &&
                              dartplant_invocation_logical_method(invocation) == nullptr &&
                              dartplant_invocation_identity_ambiguous(invocation) != 0 &&
                              dartplant_invocation_code_alias_count(invocation) == 2 &&
                              dartplant_invocation_known_code_alias_count(invocation) == 2 &&
                              dartplant_invocation_code_target_address(invocation) ==
                                  dartplant_method_runtime_address(g_instrumented_add);
    if (semantics_ok) {
        DartPlantMethodIdentityInfo first{};
        first.struct_size = sizeof(first);
        DartPlantMethodIdentityInfo second{};
        second.struct_size = sizeof(second);
        const bool aliases_ok =
            dartplant_invocation_get_code_alias(invocation, 0, &first) == DARTPLANT_OK &&
            dartplant_invocation_get_code_alias(invocation, 1, &second) == DARTPLANT_OK &&
            first.function_name != nullptr && second.function_name != nullptr &&
            ((std::strcmp(first.function_name, "instrumentedAdd") == 0 &&
              std::strcmp(second.function_name, "addInt") == 0) ||
             (std::strcmp(first.function_name, "addInt") == 0 &&
              std::strcmp(second.function_name, "instrumentedAdd") == 0));
        if (aliases_ok) g_add_int_listener_identity_ok.store(true, std::memory_order_release);
    }
    g_add_int_listener_enter.fetch_add(1, std::memory_order_relaxed);
}

void OnInstrumentedAddLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK) {
        LogFailure("instrumentedAdd result decode");
        return;
    }
    result.raw += 220;
    if (dartplant_invocation_set_result(invocation, &result) != DARTPLANT_OK) {
        LogFailure("instrumentedAdd result rewrite");
        return;
    }
    g_instrumented_add_last_result.store(result.raw, std::memory_order_relaxed);
    g_instrumented_add_leave.fetch_add(1, std::memory_order_relaxed);
}

void OnEchoObjectEnter(DartPlantInvocation* invocation, void*) {
    const uint64_t invocation_index = g_null_passthrough_count.load(std::memory_order_relaxed) +
                                      g_null_result_overrides.load(std::memory_order_relaxed);
    if (invocation_index == 0) {
        g_null_passthrough_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (invocation_index != 1) {
        LogFailure("unexpected nullable invocation count");
        g_null_semantic_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const DartPlantValue null_value = {DARTPLANT_VALUE_NULL, 0, 0};
    if (dartplant_invocation_set_result(invocation, &null_value) != DARTPLANT_OK) {
        LogFailure("nullable object result override");
        g_null_semantic_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_null_result_overrides.fetch_add(1, std::memory_order_relaxed);
}

void OnEchoObjectLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue value{};
    if (dartplant_invocation_get_result(invocation, &value) != DARTPLANT_OK ||
        value.kind != DARTPLANT_VALUE_NULL) {
        LogFailure("nullable object result decode");
        g_null_semantic_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_null_result_count.fetch_add(1, std::memory_order_relaxed);
}

void OnBoolLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue value{};
    if (dartplant_invocation_get_result(invocation, &value) != DARTPLANT_OK ||
        value.kind != DARTPLANT_VALUE_BOOL || value.raw > 1) {
        LogFailure("bool result semantic decode");
        g_bool_semantic_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (value.raw != 0) {
        g_bool_true_results.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_bool_false_results.fetch_add(1, std::memory_order_relaxed);
    }

    const DartPlantValue replacement = {DARTPLANT_VALUE_BOOL, 0, value.raw == 0 ? 1ULL : 0ULL};
    if (dartplant_invocation_set_result(invocation, &replacement) != DARTPLANT_OK) {
        LogFailure("bool result semantic encode");
        g_bool_semantic_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

[[maybe_unused]] void OnObjectLeave(DartPlantInvocation* invocation, void*) {
    const uint32_t callback_count = g_object_callback_count.fetch_add(1);
    // The leave callback runs before the Dart caller has necessarily consumed
    // the return register. Release the previous result only after a later
    // leave proves that the preceding call has returned to Dart.
    if (g_replacement_object_handle != nullptr) {
        const DartPlantStatus release_status =
            dartplant_object_release(g_replacement_object_handle);
        __android_log_print(ANDROID_LOG_INFO, kTag, "previous persistent result released status=%d",
                            release_status);
        g_replacement_object_handle = nullptr;
    }
    DartPlantValue value{};
    if (callback_count < 8 && dartplant_invocation_get_result(invocation, &value) == DARTPLANT_OK) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "object result raw=0x%llx",
                            static_cast<unsigned long long>(value.raw));
        // Observation only. The raw value is valid for this callback lifetime.
    }
    if (!g_object_bridge_probe.test_and_set()) {
        DartPlantObjectHandle* handle = nullptr;
        const DartPlantStatus status =
            dartplant_invocation_retain_result_object(invocation, DARTPLANT_OBJECT_STRONG, &handle);
        __android_log_print(ANDROID_LOG_INFO, kTag, "object handle probe status=%d error=%s",
                            status, dartplant_last_error());
        if (status == DARTPLANT_OK) {
            const DartPlantStatus replacement_status =
                dartplant_invocation_set_result_object(invocation, handle);
            uint64_t replacement_raw = 0;
            const DartPlantStatus raw_status = dartplant_object_to_raw(handle, &replacement_raw);
            __android_log_print(
                ANDROID_LOG_INFO, kTag,
                "object persistent result replacement status=%d raw_status=%d raw=0x%llx match=%u",
                replacement_status, raw_status, static_cast<unsigned long long>(replacement_raw),
                static_cast<unsigned>(raw_status == DARTPLANT_OK && replacement_raw == value.raw));
            // Keep this root until the next leave callback.
            g_replacement_object_handle = handle;
        }
        DartPlantObjectHandle* weak = nullptr;
        const DartPlantStatus weak_status =
            dartplant_invocation_retain_result_object(invocation, DARTPLANT_OBJECT_WEAK, &weak);
        if (weak_status == DARTPLANT_OK) {
            g_weak_object_handle = weak;
            uint8_t alive = 0;
            dartplant_object_is_alive(weak, &alive);
            __android_log_print(ANDROID_LOG_INFO, kTag, "weak object retained alive=%u",
                                static_cast<unsigned>(alive));
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "weak object awaiting Dart allocation-pressure GC");
        }
    } else if (g_weak_object_handle != nullptr) {
        uint8_t alive = 0;
        const DartPlantStatus alive_status =
            dartplant_object_is_alive(g_weak_object_handle, &alive);
        __android_log_print(ANDROID_LOG_INFO, kTag, "weak object after gc probe status=%d alive=%u",
                            alive_status, static_cast<unsigned>(alive));
        if (alive_status == DARTPLANT_OK && alive == 0) {
            dartplant_object_release(g_weak_object_handle);
            g_weak_object_handle = nullptr;
        }
    }
}

[[maybe_unused]] void OnDoubleLeave(DartPlantInvocation* invocation, void*) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "double leave callback");
    DartPlantValue value{};
    if (dartplant_invocation_get_result(invocation, &value) == DARTPLANT_OK) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "double result raw=0x%llx",
                            static_cast<unsigned long long>(value.raw));
        // Observation only. The raw value is valid for this callback lifetime.
    }
}

bool FindLiveTopLevelMethod(const char* name, DartPlantMethod** out_method) {
    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "Global",
        .function_name = name,
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    return dartplant_runtime_find_method(g_runtime, &query, out_method) == DARTPLANT_OK;
}

DartPlantStatus InstallMethodHook(DartPlantMethod* method, const DartPlantRuntimeProfile& profile,
                                  DartPlantInvocationCallback on_leave, DartPlantHook** out_hook,
                                  DartPlantInvocationCallback on_enter = nullptr,
                                  uint32_t flags = 0) {
    DartPlantHookOptions options = {
        .struct_size = sizeof(options),
        .flags = flags,
        .on_enter = on_enter,
        .on_leave = on_leave,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    return dartplant_runtime_hook_method_with_profile(g_runtime, method, &profile, &options,
                                                      out_hook);
}

void CompleteBootstrap(DartPlantStatus bootstrap_status, DartPlantLiveVmBootstrapInfo bootstrap,
                       const char* source) {
    __android_log_print(
        bootstrap_status == DARTPLANT_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
        "cold bootstrap status=%d source=%s tid=%u signal=%u rounds=%u sampled=%u captured=%u validated=%u send_fail=%u timeout=%u ui_polls=%u ui_hits=%u dart_pc=%u nonzero_thr=%u pc=0x%llx last_tid=%u last_pc=0x%llx last_thr=0x%llx last_pp=0x%llx last_heap=0x%llx last_null=0x%llx validation_error=%s error=%s",
        bootstrap_status, source, bootstrap.selected_tid, bootstrap.signal_number, bootstrap.rounds,
        bootstrap.sampled_threads, bootstrap.captured_contexts, bootstrap.validated_candidates,
        bootstrap.signal_send_failures, bootstrap.sample_timeouts, bootstrap.ui_activity_polls,
        bootstrap.ui_activity_hits, bootstrap.dart_instruction_samples,
        bootstrap.nonzero_thr_samples, static_cast<unsigned long long>(bootstrap.selected_pc),
        bootstrap.last_candidate_tid, static_cast<unsigned long long>(bootstrap.last_candidate_pc),
        static_cast<unsigned long long>(bootstrap.last_candidate_thr),
        static_cast<unsigned long long>(bootstrap.last_candidate_pp),
        static_cast<unsigned long long>(bootstrap.last_candidate_heap_bits),
        static_cast<unsigned long long>(bootstrap.last_candidate_null),
        bootstrap.last_validation_error,
        bootstrap_status == DARTPLANT_OK ? "none" : dartplant_last_error());
    if (bootstrap_status != DARTPLANT_OK) {
        g_cold_bootstrap_status.store(bootstrap_status, std::memory_order_release);
        return;
    }

    DartPlantRuntimeInfo runtime_info{};
    runtime_info.struct_size = sizeof(runtime_info);
    const DartPlantStatus info_status = dartplant_runtime_get_info(g_runtime, &runtime_info);
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "cold runtime info status=%d state=%u live_index_ready=%u profile_matched=%u modules=%u",
        info_status, static_cast<unsigned>(runtime_info.state),
        static_cast<unsigned>(runtime_info.live_function_index_ready),
        static_cast<unsigned>(runtime_info.profile_matched), runtime_info.loaded_module_count);
    if (info_status != DARTPLANT_OK || runtime_info.live_function_index_ready == 0 ||
        runtime_info.state != DARTPLANT_RUNTIME_READY) {
        g_cold_bootstrap_status.store(DARTPLANT_RUNTIME_NOT_READY, std::memory_order_release);
        return;
    }

    DartPlantLiveVmFunctionIndexInfo function_index{};
    function_index.struct_size = sizeof(function_index);
    if (dartplant_runtime_get_function_index_info(g_runtime, &function_index) != DARTPLANT_OK ||
        function_index.function_count == 0 || function_index.code_target_count == 0) {
        LogFailure("automatic live Function index");
        g_cold_bootstrap_status.store(DARTPLANT_RUNTIME_NOT_READY, std::memory_order_release);
        return;
    }
    bool indexed_instrumented_add = false;
    bool indexed_echo_object = false;
    uint64_t indexed_entry_va = 0;
    for (uint32_t position = 0; position < function_index.function_count; ++position) {
        DartPlantLiveVmFunctionInfo function{};
        function.struct_size = sizeof(function);
        if (dartplant_runtime_get_function_info(g_runtime, position, &function) != DARTPLANT_OK) {
            LogFailure("live FunctionInfo enumeration");
            g_cold_bootstrap_status.store(DARTPLANT_RUNTIME_NOT_READY, std::memory_order_release);
            return;
        }
        if (std::strcmp(function.library_uri, "package:dartplant_fixture/main.dart") == 0 &&
            std::strcmp(function.class_name, "Global") == 0 &&
            std::strcmp(function.function_name, "instrumentedAdd") == 0) {
            indexed_instrumented_add =
                function.function != 0 && function.code != 0 && function.code_size != 0 &&
                function.entry_alias_count == 2 &&
                function.entry_va + g_snapshot_info.load_bias == function.function_entry_point;
            indexed_entry_va = function.entry_va;
        }
        if (std::strcmp(function.library_uri, "package:dartplant_fixture/main.dart") == 0 &&
            std::strcmp(function.class_name, "Global") == 0 &&
            std::strcmp(function.function_name, "nullableEchoObject") == 0) {
            indexed_echo_object = function.function != 0 && function.code != 0 &&
                                  function.code_size != 0 && function.entry_alias_count == 1;
        }
    }
    uint32_t decoded_pool_entries = 0;
    for (uint32_t index = 0; index < 32; ++index) {
        DartPlantObjectPoolEntryInfo entry{};
        entry.struct_size = sizeof(entry);
        uint64_t offset = 0;
        uint32_t round_trip_index = UINT32_MAX;
        if (dartplant_runtime_read_global_object_pool_entry(g_runtime, index, &entry) !=
                DARTPLANT_OK ||
            entry.type == DARTPLANT_OBJECT_POOL_UNKNOWN ||
            entry.patchable != static_cast<uint8_t>(((entry.entry_bits >> 4) & 0x1) == 0) ||
            entry.snapshot_behavior != static_cast<uint8_t>((entry.entry_bits >> 5) & 0x7) ||
            dartplant_live_vm_object_pool_offset_from_index(&g_snapshot_info, index, &offset) !=
                DARTPLANT_OK ||
            offset != entry.byte_offset ||
            dartplant_live_vm_object_pool_index_from_offset(&g_snapshot_info, offset,
                                                            &round_trip_index) != DARTPLANT_OK ||
            round_trip_index != index) {
            LogFailure("ObjectPool index-space decode");
            g_cold_bootstrap_status.store(DARTPLANT_PROFILE_MISMATCH, std::memory_order_release);
            return;
        }
        ++decoded_pool_entries;
    }
    if (!indexed_instrumented_add || !indexed_echo_object) {
        LogFailure("live FunctionInfo entry_va verification");
        g_cold_bootstrap_status.store(DARTPLANT_METHOD_NOT_FOUND, std::memory_order_release);
        return;
    }
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "live-index functions=%u code_targets=%u shared_targets=%u skipped=%u instrumented_entry_va=0x%llx pool_decoded=%u",
        function_index.function_count, function_index.code_target_count,
        function_index.shared_code_target_count, function_index.skipped_function_count,
        static_cast<unsigned long long>(indexed_entry_va), decoded_pool_entries);

    if (!FindLiveTopLevelMethod("instrumentedAdd", &g_instrumented_add)) {
        LogFailure("live VM method resolution");
        g_cold_bootstrap_status.store(DARTPLANT_METHOD_NOT_FOUND, std::memory_order_release);
        return;
    }

    DartPlantRuntimeProfile instrumented_add_profile{};
    dartplant_runtime_profile_init_arm64_aot(&instrumented_add_profile);
    instrumented_add_profile.flags =
        DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT |
        DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS | DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    instrumented_add_profile.argument_count = 2;
    instrumented_add_profile.argument_locations[0] = {DARTPLANT_ABI_GP_REGISTER, 1, {0, 0}};
    instrumented_add_profile.argument_locations[1] = {DARTPLANT_ABI_GP_REGISTER, 2, {0, 0}};
    instrumented_add_profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
    DartPlantHook* rejected_hook = nullptr;
    const DartPlantStatus fail_closed_status =
        InstallMethodHook(g_instrumented_add, instrumented_add_profile, OnInstrumentedAddLeave,
                          &rejected_hook, OnInstrumentedAddEnter);
    if (fail_closed_status != DARTPLANT_SHARED_CODE_ENTRY || rejected_hook != nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "shared-code fail-closed check failed status=%d hook=%p error=%s",
                            fail_closed_status, rejected_hook, dartplant_last_error());
        g_cold_bootstrap_status.store(DARTPLANT_HOOK_FAILED, std::memory_order_release);
        return;
    }
    g_shared_policy_ok.store(true, std::memory_order_release);

    const DartPlantStatus hook_status = InstallMethodHook(
        g_instrumented_add, instrumented_add_profile, OnInstrumentedAddLeave,
        &g_instrumented_add_hook, OnInstrumentedAddEnter, DARTPLANT_HOOK_ALLOW_SHARED_CODE);
    if (hook_status != DARTPLANT_OK) {
        LogFailure("shared-code hook installation");
        g_cold_bootstrap_status.store(hook_status, std::memory_order_release);
        return;
    }

    const DartPlantMethodQuery add_int_query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "DartPlantFixture",
        .function_name = "addInt",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    if (dartplant_runtime_find_method(g_runtime, &add_int_query, &g_add_int) != DARTPLANT_OK ||
        g_add_int == nullptr || g_add_int->function == nullptr ||
        g_add_int->function->code_target != g_instrumented_add->function->code_target) {
        LogFailure("shared addInt live resolution");
        g_cold_bootstrap_status.store(DARTPLANT_METHOD_NOT_FOUND, std::memory_order_release);
        return;
    }
    DartPlantHookOptions add_int_listener_options = {
        .struct_size = sizeof(DartPlantHookOptions),
        .flags = DARTPLANT_HOOK_ALLOW_SHARED_CODE,
        .on_enter = OnSharedAddIntListenerEnter,
        .on_leave = nullptr,
        .user_data = nullptr,
        .vm_adapter = nullptr,
    };
    const DartPlantStatus listener_status = dartplant_runtime_add_listener(
        g_runtime, g_add_int, &add_int_listener_options, -100, &g_add_int_listener);
    if (listener_status != DARTPLANT_OK) {
        LogFailure("shared addInt listener");
        g_cold_bootstrap_status.store(listener_status, std::memory_order_release);
        return;
    }

    if (!FindLiveTopLevelMethod("nullableEchoObject", &g_echo_object)) {
        LogFailure("nullable object live resolution");
        g_cold_bootstrap_status.store(DARTPLANT_METHOD_NOT_FOUND, std::memory_order_release);
        return;
    }
    DartPlantRuntimeProfile echo_object_profile{};
    dartplant_runtime_profile_init_arm64_aot(&echo_object_profile);
    echo_object_profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS |
                                DARTPLANT_PROFILE_RAW_GP_RESULT |
                                DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    // Optimized nullable object arguments are not assumed to remain in a GP
    // register at this entry. This fixture validates semantic result handling.
    echo_object_profile.argument_count = 0;
    echo_object_profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
    const DartPlantStatus echo_hook_status =
        InstallMethodHook(g_echo_object, echo_object_profile, OnEchoObjectLeave,
                          &g_echo_object_hook, OnEchoObjectEnter);
    if (echo_hook_status != DARTPLANT_OK) {
        LogFailure("nullable object semantic hook");
        g_cold_bootstrap_status.store(echo_hook_status, std::memory_order_release);
        return;
    }

    if (!FindLiveTopLevelMethod("negateBool", &g_negate_bool)) {
        LogFailure("bool live resolution");
        g_cold_bootstrap_status.store(DARTPLANT_METHOD_NOT_FOUND, std::memory_order_release);
        return;
    }

    DartPlantDartFunctionSignatureInfo bool_signature{};
    bool_signature.struct_size = sizeof(bool_signature);
    DartPlantDartParameterInfo bool_parameter{};
    bool_parameter.struct_size = sizeof(bool_parameter);
    DartPlantLiveVmProfile live_profile{};
    live_profile.struct_size = sizeof(live_profile);
    const DartPlantStatus signature_status =
        dartplant_runtime_get_method_signature(g_runtime, g_negate_bool, &bool_signature);
    const DartPlantStatus parameter_status =
        dartplant_runtime_get_method_parameter(g_runtime, g_negate_bool, 0, &bool_parameter);
    const DartPlantStatus profile_status =
        dartplant_live_vm_select_profile(&g_snapshot_info, &live_profile);
    const uint32_t expected_bool_cid = live_profile.profile_version == 3 ? 63U : 62U;
    const bool signature_ok =
        signature_status == DARTPLANT_OK && parameter_status == DARTPLANT_OK &&
        profile_status == DARTPLANT_OK && bool_signature.parameter_count == 1 &&
        bool_signature.implicit_parameter_count == 0 && bool_signature.fixed_parameter_count == 1 &&
        bool_signature.optional_parameter_count == 0 && bool_signature.type_parameter_count == 0 &&
        bool_signature.parent_type_argument_count == 0 &&
        !bool_signature.has_named_optional_parameters &&
        bool_signature.result_type.kind == DARTPLANT_DART_TYPE_INTERFACE &&
        bool_signature.result_type.nullability == DARTPLANT_DART_NULLABILITY_NON_NULLABLE &&
        bool_signature.result_type.type_class_id == expected_bool_cid &&
        bool_parameter.kind == DARTPLANT_DART_PARAMETER_REQUIRED_POSITIONAL &&
        bool_parameter.is_required && bool_parameter.type.kind == DARTPLANT_DART_TYPE_INTERFACE &&
        bool_parameter.type.nullability == DARTPLANT_DART_NULLABILITY_NON_NULLABLE &&
        bool_parameter.type.type_class_id == expected_bool_cid;
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "FunctionType semantic probe status=%d parameter_status=%d profile=%u params=%u implicit=%u fixed=%u optional=%u result_cid=%u parameter_cid=%u passed=%u",
        signature_status, parameter_status, live_profile.profile_version,
        bool_signature.parameter_count, bool_signature.implicit_parameter_count,
        bool_signature.fixed_parameter_count, bool_signature.optional_parameter_count,
        bool_signature.result_type.type_class_id, bool_parameter.type.type_class_id,
        static_cast<unsigned>(signature_ok));
    __android_log_print(ANDROID_LOG_INFO, kTag, "DartPlant FunctionType semantic probe: %u",
                        static_cast<unsigned>(signature_ok));
    if (!signature_ok) {
        LogFailure("FunctionType semantic parser");
        g_cold_bootstrap_status.store(
            signature_status != DARTPLANT_OK ? signature_status : DARTPLANT_PROFILE_MISMATCH,
            std::memory_order_release);
        return;
    }

    if (!FindLiveTopLevelMethod("signatureProbe", &g_signature_probe)) {
        LogFailure("generic FunctionType live resolution");
        g_cold_bootstrap_status.store(DARTPLANT_METHOD_NOT_FOUND, std::memory_order_release);
        return;
    }
    DartPlantDartFunctionSignatureInfo generic_signature{};
    generic_signature.struct_size = sizeof(generic_signature);
    DartPlantDartParameterInfo generic_parameters[3] = {};
    for (auto& parameter : generic_parameters) parameter.struct_size = sizeof(parameter);
    const DartPlantStatus generic_signature_status =
        dartplant_runtime_get_method_signature(g_runtime, g_signature_probe, &generic_signature);
    DartPlantStatus generic_parameter_status = DARTPLANT_OK;
    for (uint32_t index = 0; index < 3 && generic_parameter_status == DARTPLANT_OK; ++index) {
        generic_parameter_status = dartplant_runtime_get_method_parameter(
            g_runtime, g_signature_probe, index, &generic_parameters[index]);
    }
    const auto& generic_value = generic_parameters[0];
    const bool type_parameter_matches =
        generic_signature.result_type.kind == DARTPLANT_DART_TYPE_PARAMETER &&
        generic_value.type.kind == DARTPLANT_DART_TYPE_PARAMETER &&
        generic_signature.result_type.is_function_type_parameter &&
        generic_value.type.is_function_type_parameter &&
        generic_signature.result_type.type_parameter_base ==
            generic_value.type.type_parameter_base &&
        generic_signature.result_type.type_parameter_index ==
            generic_value.type.type_parameter_index;
    bool enabled_ok = false;
    bool count_ok = false;
    for (uint32_t index = 1; index < 3; ++index) {
        const auto& parameter = generic_parameters[index];
        if (std::strcmp(parameter.name, "enabled") == 0) {
            enabled_ok = parameter.kind == DARTPLANT_DART_PARAMETER_NAMED &&
                         parameter.is_required &&
                         parameter.type.kind == DARTPLANT_DART_TYPE_INTERFACE &&
                         parameter.type.nullability == DARTPLANT_DART_NULLABILITY_NON_NULLABLE &&
                         parameter.type.type_class_id == expected_bool_cid;
        } else if (std::strcmp(parameter.name, "count") == 0) {
            count_ok = parameter.kind == DARTPLANT_DART_PARAMETER_NAMED && !parameter.is_required &&
                       parameter.type.kind == DARTPLANT_DART_TYPE_INTERFACE &&
                       parameter.type.nullability == DARTPLANT_DART_NULLABILITY_NON_NULLABLE;
        }
    }
    const bool generic_signature_ok =
        generic_signature_status == DARTPLANT_OK && generic_parameter_status == DARTPLANT_OK &&
        generic_signature.parameter_count == 3 && generic_signature.implicit_parameter_count == 0 &&
        generic_signature.fixed_parameter_count == 1 &&
        generic_signature.optional_parameter_count == 2 &&
        generic_signature.type_parameter_count == 1 &&
        generic_signature.parent_type_argument_count == 0 &&
        generic_signature.has_named_optional_parameters &&
        generic_value.kind == DARTPLANT_DART_PARAMETER_REQUIRED_POSITIONAL &&
        generic_value.is_required && type_parameter_matches && enabled_ok && count_ok;
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "FunctionType named semantic probe signature_status=%d parameter_status=%d params=%u fixed=%u optional=%u type_params=%u names=%s/%s required=%u/%u passed=%u",
        generic_signature_status, generic_parameter_status, generic_signature.parameter_count,
        generic_signature.fixed_parameter_count, generic_signature.optional_parameter_count,
        generic_signature.type_parameter_count, generic_parameters[1].name,
        generic_parameters[2].name, static_cast<unsigned>(generic_parameters[1].is_required),
        static_cast<unsigned>(generic_parameters[2].is_required),
        static_cast<unsigned>(generic_signature_ok));
    __android_log_print(ANDROID_LOG_INFO, kTag, "DartPlant FunctionType named semantic probe: %u",
                        static_cast<unsigned>(generic_signature_ok));
    if (!generic_signature_ok) {
        LogFailure("generic/named FunctionType semantic parser");
        g_cold_bootstrap_status.store(generic_signature_status != DARTPLANT_OK
                                          ? generic_signature_status
                                          : DARTPLANT_PROFILE_MISMATCH,
                                      std::memory_order_release);
        return;
    }

    DartPlantRuntimeProfile bool_profile{};
    dartplant_runtime_profile_init_arm64_aot(&bool_profile);
    bool_profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT |
                         DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    // This milestone proves result semantics only. Argument locations remain
    // unavailable until typed Function/ABI metadata is implemented.
    bool_profile.argument_count = 0;
    bool_profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
    const DartPlantStatus bool_status =
        InstallMethodHook(g_negate_bool, bool_profile, OnBoolLeave, &g_negate_bool_hook, nullptr,
                          DARTPLANT_HOOK_ALLOW_SHARED_CODE);
    if (bool_status != DARTPLANT_OK) {
        LogFailure("bool semantic callback installation");
        g_cold_bootstrap_status.store(bool_status, std::memory_order_release);
        return;
    }

    g_runtime_live_vm_ready.store(true, std::memory_order_release);
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "live-vm hook ready method=%s/%s/%s entry=0x%llx function=0x%llx code=0x%llx aliases=%u known_aliases=%u shared=%u explicit_opt_in=1 fail_closed=1 second_listener=1",
        g_instrumented_add->record.library_uri.c_str(),
        g_instrumented_add->record.class_name.c_str(),
        g_instrumented_add->record.function_name.c_str(),
        static_cast<unsigned long long>(dartplant_method_runtime_address(g_instrumented_add)),
        static_cast<unsigned long long>(g_instrumented_add->function->function_object),
        static_cast<unsigned long long>(g_instrumented_add->function->code_object),
        g_instrumented_add->function->code_target->AliasCount(),
        g_instrumented_add->function->code_target->KnownAliasCount(),
        static_cast<unsigned>(g_instrumented_add->function->code_target->IsShared()));
    g_cold_bootstrap_status.store(DARTPLANT_OK, std::memory_order_release);
}

void RunColdBootstrap() {
    DartPlantLiveVmBootstrapInfo bootstrap{};
    bootstrap.struct_size = sizeof(bootstrap);
    const DartPlantStatus status =
        dartplant_runtime_bootstrap_live_vm(g_runtime, nullptr, &bootstrap);
    CompleteBootstrap(status, bootstrap, "sampler");
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void dartplant_fixture_begin_object_probe() {
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "Object probe unavailable: no engine-owned VM object shim");
}

extern "C" __attribute__((visibility("default"))) void dartplant_fixture_release_object_root() {
    if (g_replacement_object_handle == nullptr) return;
    const DartPlantStatus status = dartplant_object_release(g_replacement_object_handle);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "Dart consumed object result; strong root released status=%d", status);
    g_replacement_object_handle = nullptr;
}

extern "C" __attribute__((visibility("default"))) void
dartplant_fixture_reset_instrumented_add_probe() {
    g_instrumented_add_enter.store(0, std::memory_order_relaxed);
    g_instrumented_add_leave.store(0, std::memory_order_relaxed);
    g_instrumented_add_last_result.store(0, std::memory_order_relaxed);
    g_live_vm_probe_ok.store(0, std::memory_order_relaxed);
    g_live_vm_probe_failed.store(0, std::memory_order_relaxed);
    g_shared_identity_ambiguous_seen.store(false, std::memory_order_release);
    g_add_int_listener_enter.store(0, std::memory_order_relaxed);
    g_add_int_listener_identity_ok.store(false, std::memory_order_release);
}

extern "C" __attribute__((visibility("default"))) void
dartplant_fixture_reset_null_semantic_probe() {
    g_null_passthrough_count.store(0, std::memory_order_relaxed);
    g_null_result_overrides.store(0, std::memory_order_relaxed);
    g_null_result_count.store(0, std::memory_order_relaxed);
    g_null_semantic_failures.store(0, std::memory_order_relaxed);
}

extern "C" __attribute__((visibility("default"))) uint64_t dartplant_fixture_null_semantic_probe() {
    const uint64_t passthrough = g_null_passthrough_count.load(std::memory_order_relaxed);
    const uint64_t result_overrides = g_null_result_overrides.load(std::memory_order_relaxed);
    const uint64_t null_results = g_null_result_count.load(std::memory_order_relaxed);
    const uint64_t failures = g_null_semantic_failures.load(std::memory_order_relaxed);
    const bool passed =
        passthrough == 1 && result_overrides == 1 && null_results == 2 && failures == 0;
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "null semantic probe passthrough=%llu result_overrides=%llu null_results=%llu failures=%llu passed=%u",
        static_cast<unsigned long long>(passthrough),
        static_cast<unsigned long long>(result_overrides),
        static_cast<unsigned long long>(null_results), static_cast<unsigned long long>(failures),
        static_cast<unsigned>(passed));
    return passed ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) void
dartplant_fixture_reset_bool_semantic_probe() {
    g_bool_true_results.store(0, std::memory_order_relaxed);
    g_bool_false_results.store(0, std::memory_order_relaxed);
    g_bool_semantic_failures.store(0, std::memory_order_relaxed);
}

extern "C" __attribute__((visibility("default"))) uint64_t dartplant_fixture_bool_semantic_probe() {
    const uint64_t true_results = g_bool_true_results.load(std::memory_order_relaxed);
    const uint64_t false_results = g_bool_false_results.load(std::memory_order_relaxed);
    const uint64_t failures = g_bool_semantic_failures.load(std::memory_order_relaxed);
    const bool passed = true_results == 1 && false_results == 1 && failures == 0;
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "bool semantic probe true=%llu false=%llu failures=%llu passed=%u",
                        static_cast<unsigned long long>(true_results),
                        static_cast<unsigned long long>(false_results),
                        static_cast<unsigned long long>(failures), static_cast<unsigned>(passed));
    return passed ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) uint64_t
dartplant_fixture_instrumented_add_probe() {
    const uint64_t enter = g_instrumented_add_enter.load(std::memory_order_relaxed);
    const uint64_t leave = g_instrumented_add_leave.load(std::memory_order_relaxed);
    const uint64_t result = g_instrumented_add_last_result.load(std::memory_order_relaxed) >> 1;
    const uint64_t live_ok = g_live_vm_probe_ok.load(std::memory_order_relaxed);
    const uint64_t live_failed = g_live_vm_probe_failed.load(std::memory_order_relaxed);
    const bool shared_policy_ok = g_shared_policy_ok.load(std::memory_order_acquire);
    const bool ambiguous_seen = g_shared_identity_ambiguous_seen.load(std::memory_order_acquire);
    const uint64_t add_int_listener_enter =
        g_add_int_listener_enter.load(std::memory_order_relaxed);
    const bool add_int_listener_identity_ok =
        g_add_int_listener_identity_ok.load(std::memory_order_acquire);

    DartPlantMethod* add_int = nullptr;
    const DartPlantMethodQuery add_int_query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "DartPlantFixture",
        .function_name = "addInt",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    const DartPlantStatus lookup_status =
        g_runtime_live_vm_ready.load(std::memory_order_acquire)
            ? dartplant_runtime_find_method(g_runtime, &add_int_query, &add_int)
            : DARTPLANT_RUNTIME_NOT_READY;
    const uintptr_t add_int_entry =
        add_int == nullptr ? 0 : dartplant_method_runtime_address(add_int);
    const bool lookup_ok = lookup_status == DARTPLANT_OK && add_int_entry != 0;
    const auto bootstrap_function =
        g_instrumented_add == nullptr ? nullptr : g_instrumented_add->function;
    const auto add_int_function = add_int == nullptr ? nullptr : add_int->function;
    const auto bootstrap_target =
        bootstrap_function == nullptr ? nullptr : bootstrap_function->code_target;
    const auto add_int_target =
        add_int_function == nullptr ? nullptr : add_int_function->code_target;
    const bool model_ok =
        lookup_ok && bootstrap_function != nullptr && add_int_function != nullptr &&
        bootstrap_target != nullptr && add_int_target != nullptr &&
        bootstrap_function->function_object != 0 && add_int_function->function_object != 0 &&
        bootstrap_function->function_object != add_int_function->function_object &&
        bootstrap_function->code_object != 0 &&
        bootstrap_function->code_object == add_int_function->code_object &&
        bootstrap_target == add_int_target && bootstrap_target->IsShared() &&
        bootstrap_target->AliasCount() == 2 && bootstrap_target->KnownAliasCount() == 2 &&
        shared_policy_ok && ambiguous_seen && add_int_listener_enter == enter &&
        add_int_listener_identity_ok && g_instrumented_add_hook != nullptr &&
        g_instrumented_add_hook->code_target == bootstrap_target &&
        bootstrap_target->HookRecord() == g_instrumented_add_hook;
    if (lookup_ok) {
        __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "runtime live-vm lookup addInt ok entry=0x%llx model_ok=%u function=0x%llx bootstrap_function=0x%llx code=0x%llx target=%p aliases=%u shared=%u hook_target_same=%u",
            static_cast<unsigned long long>(add_int_entry), static_cast<unsigned>(model_ok),
            static_cast<unsigned long long>(add_int_function->function_object),
            static_cast<unsigned long long>(bootstrap_function->function_object),
            static_cast<unsigned long long>(add_int_function->code_object),
            static_cast<void*>(add_int_target.get()), add_int_target->AliasCount(),
            static_cast<unsigned>(add_int_target->IsShared()),
            static_cast<unsigned>(g_instrumented_add_hook != nullptr &&
                                  g_instrumented_add_hook->code_target == add_int_target &&
                                  add_int_target->HookRecord() == g_instrumented_add_hook));
    } else {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime live-vm lookup addInt failed status=%d error=%s",
                            lookup_status, dartplant_last_error());
    }
    if (add_int != nullptr) dartplant_release_method(add_int);

    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "instrumentedAdd probe enter=%llu leave=%llu second_listener_enter=%llu live_ok=%llu live_failed=%llu lookup_ok=%u model_ok=%u shared=1 explicit_opt_in=%u ambiguous_identity=%u second_listener_identity=%u aliases=2 known_aliases=2 result=%llu expected=115",
        static_cast<unsigned long long>(enter), static_cast<unsigned long long>(leave),
        static_cast<unsigned long long>(add_int_listener_enter),
        static_cast<unsigned long long>(live_ok), static_cast<unsigned long long>(live_failed),
        static_cast<unsigned>(lookup_ok), static_cast<unsigned>(model_ok),
        static_cast<unsigned>(shared_policy_ok), static_cast<unsigned>(ambiguous_seen),
        static_cast<unsigned>(add_int_listener_identity_ok),
        static_cast<unsigned long long>(result));
    return enter != 0 && enter == leave && live_ok == enter && live_failed == 0 && lookup_ok &&
                   model_ok
               ? result
               : 0;
}

extern "C" __attribute__((visibility("default"))) int32_t
dartplant_fixture_cold_bootstrap_status() {
    return g_cold_bootstrap_status.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("hidden"))) int dartplant_fixture_initialize_with_registers(
    void* api_dl_data, uint64_t null_value, uint64_t thr, uint64_t pp, uint64_t heap_bits) {
    if (api_dl_data == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    if (g_runtime != nullptr) return DARTPLANT_OK;
    (void) api_dl_data;

    const DartPlantNativeApiEntries* host_api = dartplant_fixture_standalone_dobby_host();
    dartplant::InstallHostApi(host_api);
    dartplant::RefreshModules();

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    if (dartplant_runtime_create(&profile, &g_runtime) != DARTPLANT_OK ||
        dartplant_runtime_on_module_loaded(g_runtime, "libapp.so", nullptr) != DARTPLANT_OK) {
        LogFailure("runtime initialization");
        const int32_t status = DARTPLANT_RUNTIME_NOT_READY;
        g_cold_bootstrap_status.store(status, std::memory_order_release);
        return status;
    }

    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    const DartPlantStatus snapshot_status =
        dartplant_runtime_get_flutter_snapshot(g_runtime, &snapshot_info);
    if (snapshot_status != DARTPLANT_OK) {
        LogFailure("snapshot discovery");
        g_cold_bootstrap_status.store(snapshot_status, std::memory_order_release);
        return snapshot_status;
    }
    g_snapshot_info = snapshot_info;

    DartPlantRuntimeInfo runtime_info{};
    runtime_info.struct_size = sizeof(runtime_info);
    const DartPlantStatus info_status = dartplant_runtime_get_info(g_runtime, &runtime_info);
    __android_log_print(
        ANDROID_LOG_INFO, kTag,
        "runtime init snapshot=%s profile=%s state=%u live_index_ready=%u profile_matched=%u",
        snapshot_info.snapshot_hash, snapshot_info.profile_name,
        static_cast<unsigned>(runtime_info.state),
        static_cast<unsigned>(runtime_info.live_function_index_ready),
        static_cast<unsigned>(runtime_info.profile_matched));
    if (info_status != DARTPLANT_OK || runtime_info.live_function_index_ready != 0 ||
        runtime_info.state != DARTPLANT_RUNTIME_IMAGES_READY) {
        g_cold_bootstrap_status.store(DARTPLANT_RUNTIME_NOT_READY, std::memory_order_release);
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    g_cold_bootstrap_status.store(-1, std::memory_order_release);
    g_runtime_live_vm_ready.store(false, std::memory_order_release);
    g_shared_policy_ok.store(false, std::memory_order_release);
    g_shared_identity_ambiguous_seen.store(false, std::memory_order_release);
    g_add_int_listener_enter.store(0, std::memory_order_relaxed);
    g_add_int_listener_identity_ok.store(false, std::memory_order_release);
    dartplant_fixture_reset_null_semantic_probe();
    dartplant_fixture_reset_bool_semantic_probe();

    DartPlantLiveVmArm64Registers entry_registers{};
    entry_registers.struct_size = sizeof(entry_registers);
    entry_registers.thr = thr;
    entry_registers.pp = pp;
    entry_registers.heap_bits = heap_bits;
    entry_registers.null_value = null_value;
    DartPlantLiveVmBootstrapInfo entry_bootstrap{};
    entry_bootstrap.struct_size = sizeof(entry_bootstrap);
    const DartPlantStatus entry_status = dartplant_runtime_bootstrap_live_vm_from_arm64_registers(
        g_runtime, &entry_registers, &entry_bootstrap);
    if (entry_status == DARTPLANT_OK) {
        g_cold_bootstrap_thread =
            std::thread(CompleteBootstrap, entry_status, entry_bootstrap, "ffi-entry");
    } else {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "FFI-entry LiveVmContext validation failed status=%d error=%s; "
                            "falling back to sampler",
                            entry_status, dartplant_last_error());
        g_cold_bootstrap_thread = std::thread(RunColdBootstrap);
    }
    return DARTPLANT_OK;
}

#if defined(__aarch64__)
extern "C" __attribute__((naked, visibility("default"))) int dartplant_fixture_initialize(void*) {
    __asm__ volatile(
        "mov x1, x22\n"
        "mov x2, x26\n"
        "mov x3, x27\n"
        "mov x4, x28\n"
        "b dartplant_fixture_initialize_with_registers\n");
}
#else
extern "C"
    __attribute__((visibility("default"))) int dartplant_fixture_initialize(void* api_dl_data) {
    return dartplant_fixture_initialize_with_registers(api_dl_data, 0, 0, 0, 0);
}
#endif

extern "C" __attribute__((visibility("default"))) void dartplant_fixture_shutdown() {
    if (g_cold_bootstrap_thread.joinable()) g_cold_bootstrap_thread.join();
    if (g_weak_object_handle != nullptr) {
        dartplant_object_release(g_weak_object_handle);
        g_weak_object_handle = nullptr;
    }
    if (g_replacement_object_handle != nullptr) {
        dartplant_object_release(g_replacement_object_handle);
        g_replacement_object_handle = nullptr;
    }
    if (g_add_int_listener != nullptr) {
        dartplant_remove_listener(g_add_int_listener);
        dartplant_release_listener(g_add_int_listener);
    }
    if (g_instrumented_add_hook != nullptr) {
        dartplant_unhook(g_instrumented_add_hook);
        dartplant_release_hook(g_instrumented_add_hook);
    }
    if (g_echo_object_hook != nullptr) {
        dartplant_unhook(g_echo_object_hook);
        dartplant_release_hook(g_echo_object_hook);
    }
    if (g_negate_bool_hook != nullptr) {
        dartplant_unhook(g_negate_bool_hook);
        dartplant_release_hook(g_negate_bool_hook);
    }
    if (g_add_int != nullptr) dartplant_release_method(g_add_int);
    if (g_instrumented_add != nullptr) dartplant_release_method(g_instrumented_add);
    if (g_echo_object != nullptr) dartplant_release_method(g_echo_object);
    if (g_negate_bool != nullptr) dartplant_release_method(g_negate_bool);
    if (g_signature_probe != nullptr) dartplant_release_method(g_signature_probe);
    if (g_runtime != nullptr) dartplant_runtime_destroy(g_runtime);
    g_instrumented_add = nullptr;
    g_add_int = nullptr;
    g_echo_object = nullptr;
    g_negate_bool = nullptr;
    g_signature_probe = nullptr;
    g_instrumented_add_hook = nullptr;
    g_echo_object_hook = nullptr;
    g_negate_bool_hook = nullptr;
    g_add_int_listener = nullptr;
    g_runtime = nullptr;
    g_snapshot_info = {};
    g_cold_bootstrap_status.store(-1, std::memory_order_release);
    g_runtime_live_vm_ready.store(false, std::memory_order_release);
    g_shared_policy_ok.store(false, std::memory_order_release);
    g_shared_identity_ambiguous_seen.store(false, std::memory_order_release);
    g_add_int_listener_enter.store(0, std::memory_order_relaxed);
    g_add_int_listener_identity_ok.store(false, std::memory_order_release);
    dartplant_fixture_reset_null_semantic_probe();
    dartplant_fixture_reset_bool_semantic_probe();
}
