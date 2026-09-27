// Test-only JNI/FFI entrypoints live inside the SAME libdartplant.so as the
// Native API host and exact VM adapter. No second copy of dartplant_core.
#include <android/log.h>
#include <jni.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "dart_api.h"
#include "dart_api_dl.h"
#include "dartplant/adapters/dobby.h"
#include "dartplant/adapters/flutter_vm.h"
#include "dartplant/adapters/lsposed_native_api.h"
#include "dartplant/dartplant.h"
#include "dartplant/hook.h"
#include "dartplant/runtime.h"
#include "external_root_sidecar.h"
#include "ordinary_aot_sidecar.h"
#include "p6_entry_stack_sidecar.h"
#include "p6_forced_stack_closure_sidecar.h"
#include "p6_forced_stack_sidecar.h"
#include "p6_int64_sidecar.h"
#include "p6_odd_stack_sidecar.h"
#include "p6_pair_sidecar.h"
#include "p6_throwing_stack_sidecar.h"
#include "runtime/default_runtime.h"
#include "runtime/runtime_internal.h"
#include "runtime/snapshot_index.h"
#include "type_arguments_closure_sidecar.h"
#include "vm/abi/resolver.h"

#if !defined(DARTPLANT_DART_PLANT_EXTERNAL_ROOT_ABI_EVIDENCE_AVAILABLE) ||                         \
    DARTPLANT_DART_PLANT_EXTERNAL_ROOT_ABI_EVIDENCE_AVAILABLE != 1
#error "External VM-root proof needs an exact compiler/AOT sidecar for this fixture"
#endif
#if !defined(DARTPLANT_DART_PLANT_TYPE_ARGUMENTS_CLOSURE_ABI_EVIDENCE_AVAILABLE) ||                \
    DARTPLANT_DART_PLANT_TYPE_ARGUMENTS_CLOSURE_ABI_EVIDENCE_AVAILABLE != 1
#error "External TypeArguments proof needs an exact implicit-closure sidecar"
#endif

#ifndef DARTPLANT_FIXED_VM_FAMILY
#error "Test module must embed one exact VM family"
#endif
extern "C" __attribute__((used, visibility("default"), section(".dartplant.family")))
const char dartplant_fixed_vm_family[] = DARTPLANT_FIXED_VM_FAMILY;

extern "C" uint64_t dartplant_module_external_bootstrap(uint64_t api_dl_data, uint64_t thr,
                                                        uint64_t pp, uint64_t heap_bits,
                                                        uint64_t null_value);
extern "C" uint64_t dartplant_module_external_counts();
extern "C" uint64_t dartplant_module_external_retire();
extern "C" uint64_t dartplant_module_external_exception_probe();
extern "C" uint64_t dartplant_module_external_object_root_probe();
extern "C" uint64_t dartplant_module_external_loader_callback_retire();
extern "C" int32_t dartplant_module_external_typeargs_prepare(Dart_Handle retained_closure,
                                                              Dart_Handle pressure_send_port,
                                                              uint8_t require_relocation);
extern "C" uint64_t dartplant_module_external_typeargs_probe();
extern "C" int32_t dartplant_module_external_p6_install();
extern "C" uint64_t dartplant_module_external_p6_probe();
extern "C" int32_t dartplant_module_external_closure_install();
extern "C" uint64_t dartplant_module_external_closure_probe();
extern "C" int32_t dartplant_module_external_ordinary_install();
extern "C" uint64_t dartplant_module_external_ordinary_mark_shared();
extern "C" uint64_t dartplant_module_external_ordinary_probe();
extern "C" uint64_t dartplant_module_physical_mapping_control();
extern "C" uint8_t dartplant_lsposed_native_host_ready();
extern "C" uint8_t dartplant_lsposed_native_backend_selftest();

namespace {

std::mutex g_bootstrap_mutex;
DartPlantFlutterVmAdapter* g_vm_adapter = nullptr;
DartPlantHook* g_dart_hook = nullptr;
DartPlantHook* g_null_hook = nullptr;
DartPlantHook* g_bool_hook = nullptr;
DartPlantHook* g_exception_hook = nullptr;
DartPlantHook* g_root_hook = nullptr;
DartPlantHook* g_typeargs_hook = nullptr;
DartPlantMethod* g_typeargs_method = nullptr;
DartPlantHook* g_closure_hook = nullptr;
DartPlantMethod* g_closure_method = nullptr;
DartPlantHookHandle* g_ordinary_hook = nullptr;
DartPlantHookHandle* g_ordinary_observer = nullptr;
DartPlantMethod* g_ordinary_method = nullptr;
DartPlantObjectHandle* g_strong_object = nullptr;
std::atomic_uint32_t g_enter{0};
std::atomic_uint32_t g_leave{0};
std::atomic_uint32_t g_rewrite_failures{0};
std::atomic_uint32_t g_null_enters{0};
std::atomic_uint32_t g_null_leaves{0};
std::atomic_uint32_t g_null_overrides{0};
std::atomic_uint32_t g_null_failures{0};
std::atomic_uint32_t g_bool_true{0};
std::atomic_uint32_t g_bool_false{0};
std::atomic_uint32_t g_bool_failures{0};
std::atomic_uint32_t g_exception_enter{0};
std::atomic_uint32_t g_exception_leave{0};
std::atomic_uint32_t g_exception_unwind{0};
std::atomic_bool g_exception_phase_safe{false};
std::atomic_uint32_t g_root_leave{0};
std::atomic_uint32_t g_root_failures{0};
std::atomic_bool g_root_alive{false};
std::atomic_bool g_root_relocated{false};
std::atomic_uint64_t g_root_initial_raw{0};
std::atomic<Dart_Port> g_typeargs_pressure_port{ILLEGAL_PORT};
std::atomic_uint64_t g_typeargs_enter{0};
std::atomic_uint64_t g_typeargs_failures{0};
std::atomic_uint64_t g_typeargs_gc_calls{0};
std::atomic_uint64_t g_typeargs_parameter_before{0};
std::atomic_uint64_t g_typeargs_parameter_after{0};
std::atomic_uint64_t g_typeargs_vector_before{0};
std::atomic_uint64_t g_typeargs_vector_after{0};
std::atomic_uint64_t g_typeargs_element_before{0};
std::atomic_uint64_t g_typeargs_element_after{0};
std::atomic_bool g_typeargs_parameter_relocated{false};
std::atomic_bool g_typeargs_vector_relocated{false};
std::atomic_bool g_typeargs_element_relocated{false};
std::atomic_bool g_typeargs_callback_passed{false};
std::atomic_bool g_typeargs_require_relocation{true};
std::atomic_uint32_t g_closure_enter{0};
std::atomic_uint32_t g_closure_failures{0};
std::atomic_uint32_t g_ordinary_enter{0};
std::atomic_uint32_t g_ordinary_leave{0};
std::atomic_uint32_t g_ordinary_observer_enter{0};
std::atomic_uint32_t g_ordinary_failures{0};
std::atomic_bool g_guest_backend_prepared{false};
std::atomic_bool g_deferred_before_ok{false};

bool ProveChangedSlotless(DartPlantRuntime* runtime) {
    if (runtime == nullptr) return false;
    dartplant::SnapshotIndex base;
    DartPlantLiveVmFunctionIndexInfo base_info{};
    dartplant::RuntimeImage deferred{};
    const dartplant::RuntimeProfileRecord* vm_profile = nullptr;
    {
        std::lock_guard lock(runtime->mutex);
        const auto& group = dartplant::RuntimeIsolateGroup(runtime);
        const auto* image = group.image_set.FindByLoadingUnitId(2);
        if (image == nullptr || !group.live_snapshot_index.has_value() ||
            !group.live_snapshot_index->stable_live_directory ||
            group.live_snapshot_index->live_function_infos.empty()) {
            return false;
        }
        deferred = *image;
        base = *group.live_snapshot_index;
        base_info = group.live_function_index_info;
        vm_profile = dartplant::FindRuntimeProfileByVersion(base.vm_profile_version);
    }
    if (vm_profile == nullptr || deferred.live_entry_count == 0) return false;
    auto bad_base = base;
    auto record = base.live_function_infos.front();
    record.runtime_image_id = deferred.id;
    record.runtime_image_incarnation_epoch = deferred.incarnation_epoch;
    record.engine_incarnation_epoch = deferred.engine_incarnation_epoch;
    record.isolate_group_incarnation_epoch = deferred.isolate_group_incarnation_epoch;
    record.runtime_generation = deferred.runtime_generation;
    record.loading_unit_id = deferred.loading_unit_id;
    record.owner_class_id = 0;
    record.owner_function_index = UINT32_MAX;
    bad_base.live_function_infos.push_back(record);
    auto corrupt_info = base_info;
    corrupt_info.function_count = static_cast<uint32_t>(bad_base.live_function_infos.size());
    DartPlantLiveVmFunctionIndexInfo out_info{};
    out_info.struct_size = sizeof(out_info);
    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    const std::array<uint64_t, 1> changed = {deferred.id};
    std::string error;
    const auto result = dartplant::BuildDeferredLiveSnapshotIndexIncrement(
        bad_base, context, {}, changed, *vm_profile, *vm_profile, *vm_profile, nullptr, nullptr,
        corrupt_info, &out_info, &error);
    const bool rejected =
        !result.has_value() &&
        error == "changed deferred image contains a slotless stable-directory record";
    bool unchanged = out_info.function_count == 0;
    {
        std::lock_guard lock(runtime->mutex);
        const auto& current = dartplant::RuntimeIsolateGroup(runtime);
        const auto* live = current.image_set.FindById(deferred.id);
        unchanged = unchanged && current.live_snapshot_index.has_value() &&
                    current.live_snapshot_index->live_function_infos.size() ==
                        base.live_function_infos.size() &&
                    live != nullptr && live->incarnation_epoch == deferred.incarnation_epoch &&
                    live->live_entry_count == deferred.live_entry_count;
    }
    const bool passed = rejected && unchanged;
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=changed_slotless_reject, state=%s, rejected=%u, unchanged=%u, "
        "loading_unit=%u, error=%s}",
        passed ? "pass" : "fail", static_cast<unsigned>(rejected), static_cast<unsigned>(unchanged),
        deferred.loading_unit_id, error.c_str());
    return passed;
}

uint64_t RefreshDeferredOwner(uint64_t thr, uint64_t pp, uint64_t heap_bits, uint64_t null_value) {
    DartPlantRuntime* runtime = dartplant::DefaultRuntimeInstanceForTesting();
    if (runtime == nullptr || g_vm_adapter == nullptr || g_dart_hook == nullptr ||
        !g_deferred_before_ok.load(std::memory_order_acquire)) {
        return 0;
    }
    DartPlantLiveVmArm64Registers registers{};
    registers.struct_size = sizeof(registers);
    registers.thr = thr;
    registers.pp = pp;
    registers.heap_bits = heap_bits;
    registers.null_value = null_value;
    const DartPlantStatus refresh = dartplant_runtime_refresh_modules(runtime);
    DartPlantLiveVmBootstrapInfo first{};
    first.struct_size = sizeof(first);
    const DartPlantStatus bootstrap =
        refresh == DARTPLANT_OK
            ? dartplant_runtime_bootstrap_live_vm_from_arm64_registers_with_adapter(
                  runtime, &registers, dartplant_flutter_vm_adapter_get(g_vm_adapter), &first)
            : refresh;
    uint32_t image_count = 0;
    DartPlantRuntimeImageInfo deferred{};
    deferred.struct_size = sizeof(deferred);
    bool found_deferred = false;
    if (bootstrap == DARTPLANT_OK &&
        dartplant_runtime_get_image_count(runtime, &image_count) == DARTPLANT_OK) {
        for (uint32_t index = 0; index < image_count; ++index) {
            DartPlantRuntimeImageInfo info{};
            info.struct_size = sizeof(info);
            if (dartplant_runtime_get_image_info(runtime, index, &info) == DARTPLANT_OK &&
                info.loading_unit_id == 2) {
                deferred = info;
                found_deferred = true;
                break;
            }
        }
    }
    const DartPlantMethodQuery query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/deferred_probe.dart",
        .class_name = "Global",
        .function_name = "deferredAdd",
        .signature = nullptr,
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    const DartPlantStatus lookup = bootstrap == DARTPLANT_OK
                                       ? dartplant_runtime_find_method(runtime, &query, &method)
                                       : bootstrap;
    if (method != nullptr) dartplant_release_method(method);
    const bool slotless = bootstrap == DARTPLANT_OK && ProveChangedSlotless(runtime);
    // Mirror the embedded fixture's logical unload/rebind contract. Flutter
    // keeps this loading unit mapped; this is not physical dlclose evidence.
    const DartPlantStatus unload =
        dartplant_runtime_on_module_unloading(runtime, "libapp.so-2.part.so", nullptr);
    const DartPlantStatus refresh_again =
        unload == DARTPLANT_OK ? dartplant_runtime_refresh_modules(runtime) : unload;
    DartPlantLiveVmBootstrapInfo rebound{};
    rebound.struct_size = sizeof(rebound);
    const DartPlantStatus rebootstrap =
        refresh_again == DARTPLANT_OK
            ? dartplant_runtime_bootstrap_live_vm_from_arm64_registers_with_adapter(
                  runtime, &registers, dartplant_flutter_vm_adapter_get(g_vm_adapter), &rebound)
            : refresh_again;
    DartPlantMethod* rebound_method = nullptr;
    const DartPlantStatus rebound_lookup =
        rebootstrap == DARTPLANT_OK
            ? dartplant_runtime_find_method(runtime, &query, &rebound_method)
            : rebootstrap;
    if (rebound_method != nullptr) dartplant_release_method(rebound_method);
    const bool passed = refresh == DARTPLANT_OK && bootstrap == DARTPLANT_OK && image_count >= 2 &&
                        found_deferred && deferred.kind == DARTPLANT_RUNTIME_IMAGE_DEFERRED &&
                        deferred.loading_unit_id == 2 && deferred.live_semantic_bound != 0 &&
                        lookup == DARTPLANT_OK && slotless && unload == DARTPLANT_OK &&
                        refresh_again == DARTPLANT_OK && rebootstrap == DARTPLANT_OK &&
                        rebound_lookup == DARTPLANT_OK;
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=deferred_lifecycle, state=%s, refresh=%d, bootstrap=%d, "
        "image_count=%u, found=%u, lookup=%d, slotless=%u, unload=%d, rebootstrap=%d, "
        "rebound_lookup=%d}",
        passed ? "pass" : "fail", refresh, bootstrap, image_count,
        static_cast<unsigned>(found_deferred), lookup, static_cast<unsigned>(slotless), unload,
        rebootstrap, rebound_lookup);
    return passed ? 1 : 0;
}

void OnEnter(DartPlantInvocation* invocation, void*) {
    // This fixture tests return-value publication, not an unverified legacy
    // argument location. An external AOT invocation observed heap objects in
    // the declared x1/x2 slots; never write those as if they were Smi values.
    (void) invocation;
    g_enter.fetch_add(1, std::memory_order_relaxed);
}

void OnLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    const bool read_ok = dartplant_invocation_get_result(invocation, &result) == DARTPLANT_OK &&
                         result.kind == DARTPLANT_VALUE_SMI && result.raw == 10;
    const uint64_t original = result.raw;
    if (read_ok) result.raw += 220;  // Smi: 5 -> 115.
    const bool written =
        read_ok && dartplant_invocation_set_result(invocation, &result) == DARTPLANT_OK;
    if (!written) g_rewrite_failures.fetch_add(1, std::memory_order_relaxed);
    __android_log_print(written ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=result_rewrite, state=%s, before=%llu, "
                        "after=%llu, proof=result_only}",
                        written ? "pass" : "fail", static_cast<unsigned long long>(original >> 1),
                        static_cast<unsigned long long>(result.raw >> 1));
    g_leave.fetch_add(1, std::memory_order_relaxed);
}

void OnNullEnter(DartPlantInvocation* invocation, void*) {
    const uint32_t ordinal = g_null_enters.fetch_add(1, std::memory_order_relaxed);
    if (ordinal == 0) return;
    if (ordinal != 1) {
        g_null_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const DartPlantValue replacement = {DARTPLANT_VALUE_NULL, 0, 0};
    if (dartplant_invocation_set_result(invocation, &replacement) != DARTPLANT_OK) {
        g_null_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_null_overrides.fetch_add(1, std::memory_order_relaxed);
}

void OnNullLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue value{};
    const bool valid = dartplant_invocation_get_result(invocation, &value) == DARTPLANT_OK &&
                       value.kind == DARTPLANT_VALUE_NULL;
    if (!valid) {
        g_null_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_null_leaves.fetch_add(1, std::memory_order_relaxed);
}

void OnBoolLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue value{};
    if (dartplant_invocation_get_result(invocation, &value) != DARTPLANT_OK ||
        value.kind != DARTPLANT_VALUE_BOOL || value.raw > 1) {
        g_bool_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const DartPlantValue replacement = {DARTPLANT_VALUE_BOOL, 0, value.raw == 0 ? 1ULL : 0ULL};
    if (dartplant_invocation_set_result(invocation, &replacement) != DARTPLANT_OK) {
        g_bool_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (value.raw == 0) {
        g_bool_false.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_bool_true.fetch_add(1, std::memory_order_relaxed);
    }
}

bool SemanticProbe() {
    const uint32_t null_enters = g_null_enters.load(std::memory_order_acquire);
    const uint32_t null_leaves = g_null_leaves.load(std::memory_order_acquire);
    const uint32_t null_overrides = g_null_overrides.load(std::memory_order_acquire);
    const uint32_t null_failures = g_null_failures.load(std::memory_order_acquire);
    const uint32_t true_results = g_bool_true.load(std::memory_order_acquire);
    const uint32_t false_results = g_bool_false.load(std::memory_order_acquire);
    const uint32_t bool_failures = g_bool_failures.load(std::memory_order_acquire);
    const bool passed = null_enters == 2 && null_leaves == 2 && null_overrides == 1 &&
                        null_failures == 0 && true_results == 1 && false_results == 1 &&
                        bool_failures == 0;
    __android_log_print(passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=semantic_probe, state=%s, null_enter=%u, "
                        "null_leave=%u, null_override=%u, null_failures=%u, bool_true=%u, "
                        "bool_false=%u, bool_failures=%u}",
                        passed ? "pass" : "fail", null_enters, null_leaves, null_overrides,
                        null_failures, true_results, false_results, bool_failures);
    return passed;
}

void OnExceptionEnter(DartPlantInvocation*, void*) {
    g_exception_enter.fetch_add(1, std::memory_order_relaxed);
}

void OnExceptionLeave(DartPlantInvocation*, void*) {
    g_exception_leave.fetch_add(1, std::memory_order_relaxed);
}

void OnExceptionUnwind(DartPlantInvocation* invocation, void*) {
    DartPlantValue argument{};
    DartPlantValue exception{};
    DartPlantValue stacktrace{};
    const bool phase_safe =
        dartplant_invocation_phase(invocation) == DARTPLANT_INVOCATION_EXCEPTION &&
        dartplant_invocation_get_argument(invocation, 0, &argument) ==
            DARTPLANT_INVALID_INVOCATION_PHASE &&
        invocation->vm_adapter == nullptr &&
        dartplant_invocation_get_exception(invocation, &exception) ==
            DARTPLANT_INVALID_INVOCATION_PHASE &&
        dartplant_invocation_get_stacktrace(invocation, &stacktrace) ==
            DARTPLANT_INVALID_INVOCATION_PHASE;
    g_exception_phase_safe.store(phase_safe, std::memory_order_release);
    g_exception_unwind.fetch_add(1, std::memory_order_relaxed);
}

bool ExceptionProbe() {
    const uint32_t enter = g_exception_enter.load(std::memory_order_acquire);
    const uint32_t leave = g_exception_leave.load(std::memory_order_acquire);
    const uint32_t unwind = g_exception_unwind.load(std::memory_order_acquire);
    bool hook_idle = false;
    if (g_exception_hook != nullptr) {
        std::lock_guard hook_lock(g_exception_hook->mutex);
        hook_idle = g_exception_hook->in_flight == 0 &&
                    g_exception_hook->active.load(std::memory_order_acquire);
    }
    const bool passed = enter == 1 && leave == 0 && unwind == 1 && hook_idle &&
                        g_exception_phase_safe.load(std::memory_order_acquire);
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=exception_probe, state=%s, enter=%u, "
        "leave=%u, unwind=%u, phase_safe=%u, object_api_available=0, "
        "hook_idle=%u}",
        passed ? "pass" : "fail", enter, leave, unwind,
        static_cast<unsigned>(g_exception_phase_safe.load(std::memory_order_acquire)),
        static_cast<unsigned>(hook_idle));
    return passed;
}

void OnObjectRootLeave(DartPlantInvocation* invocation, void*) {
    const uint32_t ordinal = g_root_leave.fetch_add(1, std::memory_order_relaxed);
    if (ordinal == 0) {
        DartPlantValue value{};
        DartPlantObjectHandle* handle = nullptr;
        const DartPlantStatus read_status = dartplant_invocation_get_result(invocation, &value);
        const DartPlantStatus retain_status =
            read_status == DARTPLANT_OK && value.kind == DARTPLANT_VALUE_HEAP_OBJECT
                ? dartplant_invocation_retain_result_object(invocation, DARTPLANT_OBJECT_STRONG,
                                                            &handle)
                : DARTPLANT_PROFILE_MISMATCH;
        uint64_t raw = 0;
        const DartPlantStatus raw_status = retain_status == DARTPLANT_OK
                                               ? dartplant_object_to_raw(handle, &raw)
                                               : DARTPLANT_PROFILE_MISMATCH;
        const DartPlantStatus publish_status =
            raw_status == DARTPLANT_OK ? dartplant_invocation_set_result_object(invocation, handle)
                                       : DARTPLANT_PROFILE_MISMATCH;
        if (publish_status != DARTPLANT_OK || raw == 0) {
            g_root_failures.fetch_add(1, std::memory_order_relaxed);
            if (handle != nullptr) (void) dartplant_object_release(handle);
            __android_log_print(ANDROID_LOG_ERROR, "DartPlantModule",
                                "DARTPLANT_HOST {event=object_root, state=fail, "
                                "stage=retain, read=%d, retain=%d, raw=%d, publish=%d, error=%s}",
                                read_status, retain_status, raw_status, publish_status,
                                dartplant_last_error());
            return;
        }
        g_strong_object = handle;
        g_root_initial_raw.store(raw, std::memory_order_release);
        __android_log_print(ANDROID_LOG_INFO, "DartPlantModule",
                            "DARTPLANT_HOST {event=object_root, state=retained, initial=%p}",
                            reinterpret_cast<void*>(raw));
        return;
    }
    if (ordinal == 1 && g_strong_object != nullptr) {
        uint64_t relocated = 0;
        uint8_t alive = 0;
        const DartPlantStatus raw_status = dartplant_object_to_raw(g_strong_object, &relocated);
        const DartPlantStatus alive_status = dartplant_object_is_alive(g_strong_object, &alive);
        const bool valid = raw_status == DARTPLANT_OK && alive_status == DARTPLANT_OK &&
                           alive != 0 && relocated != 0;
        if (!valid) g_root_failures.fetch_add(1, std::memory_order_relaxed);
        g_root_alive.store(valid, std::memory_order_release);
        g_root_relocated.store(
            valid && relocated != g_root_initial_raw.load(std::memory_order_acquire),
            std::memory_order_release);
        const DartPlantStatus release_status = dartplant_object_release(g_strong_object);
        if (release_status != DARTPLANT_OK) g_root_failures.fetch_add(1, std::memory_order_relaxed);
        g_strong_object = nullptr;
        __android_log_print(
            valid && release_status == DARTPLANT_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
            "DartPlantModule",
            "DARTPLANT_HOST {event=object_root, state=%s, alive=%u, "
            "relocated=%u, raw_status=%d, alive_status=%d, release_status=%d}",
            valid && release_status == DARTPLANT_OK ? "pass" : "fail", static_cast<unsigned>(valid),
            static_cast<unsigned>(g_root_relocated.load(std::memory_order_acquire)), raw_status,
            alive_status, release_status);
        return;
    }
    g_root_failures.fetch_add(1, std::memory_order_relaxed);
}

bool ObjectRootProbe() {
    const uint32_t leave = g_root_leave.load(std::memory_order_acquire);
    const uint32_t failures = g_root_failures.load(std::memory_order_acquire);
    const bool alive = g_root_alive.load(std::memory_order_acquire);
    const bool relocated = g_root_relocated.load(std::memory_order_acquire);
    const bool passed = leave == 2 && failures == 0 && alive && g_strong_object == nullptr;
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=object_root_probe, state=%s, leave=%u, "
        "failures=%u, alive=%u, relocated=%u, initial_nonzero=%u}",
        passed ? "pass" : "fail", leave, failures, static_cast<unsigned>(alive),
        static_cast<unsigned>(relocated),
        static_cast<unsigned>(g_root_initial_raw.load(std::memory_order_acquire) != 0));
    return passed;
}

bool TransitionProbe() {
    if (g_vm_adapter == nullptr) return false;
    const DartPlantFlutterVmProofState state = dartplant_flutter_vm_adapter_capability_state(
        g_vm_adapter, DARTPLANT_FLUTTER_VM_CAP_GENERATED_TRANSITION_SOURCE_VERIFIED);
    const uint64_t failed = dartplant_flutter_vm_adapter_failed_capabilities(g_vm_adapter);
    const bool passed =
        state == DARTPLANT_FLUTTER_VM_PROOF_VERIFIED &&
        (failed & DARTPLANT_FLUTTER_VM_CAP_GENERATED_TRANSITION_SOURCE_VERIFIED) == 0;
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=transition, state=%s, proof_state=%u, failed=0x%llx}",
        passed ? "pass" : "fail", static_cast<unsigned>(state),
        static_cast<unsigned long long>(failed));
    return passed;
}

void ResetTypeArgsProbe() {
    g_typeargs_enter.store(0, std::memory_order_relaxed);
    g_typeargs_failures.store(0, std::memory_order_relaxed);
    g_typeargs_gc_calls.store(0, std::memory_order_relaxed);
    g_typeargs_parameter_before.store(0, std::memory_order_relaxed);
    g_typeargs_parameter_after.store(0, std::memory_order_relaxed);
    g_typeargs_vector_before.store(0, std::memory_order_relaxed);
    g_typeargs_vector_after.store(0, std::memory_order_relaxed);
    g_typeargs_element_before.store(0, std::memory_order_relaxed);
    g_typeargs_element_after.store(0, std::memory_order_relaxed);
    g_typeargs_parameter_relocated.store(false, std::memory_order_relaxed);
    g_typeargs_vector_relocated.store(false, std::memory_order_relaxed);
    g_typeargs_element_relocated.store(false, std::memory_order_relaxed);
    g_typeargs_callback_passed.store(false, std::memory_order_relaxed);
}

void OnTypeArgsEnter(DartPlantInvocation* invocation, void*) {
    g_typeargs_enter.fetch_add(1, std::memory_order_relaxed);
    DartPlantArgumentsDescriptorInfo descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    DartPlantValue parameter_before{};
    DartPlantValue vector_before{};
    DartPlantValue element_before{};
    const DartPlantStatus descriptor_status =
        dartplant_invocation_get_arguments_descriptor(invocation, &descriptor);
    const DartPlantStatus parameter_status =
        dartplant_invocation_get_argument(invocation, 0, &parameter_before);
    const DartPlantStatus vector_status =
        dartplant_invocation_get_closure_type_arguments(invocation, &vector_before);
    const DartPlantStatus element_status =
        dartplant_invocation_get_closure_type_argument(invocation, 0, &element_before);
    const bool before_ok =
        descriptor_status == DARTPLANT_OK && descriptor.type_args_len == 1 &&
        descriptor.named_count == 2 && parameter_status == DARTPLANT_OK &&
        parameter_before.kind == DARTPLANT_VALUE_HEAP_OBJECT && parameter_before.raw != 0 &&
        vector_status == DARTPLANT_OK && vector_before.kind == DARTPLANT_VALUE_HEAP_OBJECT &&
        vector_before.raw != 0 && element_status == DARTPLANT_OK &&
        element_before.kind == DARTPLANT_VALUE_HEAP_OBJECT && element_before.raw != 0;
    g_typeargs_parameter_before.store(parameter_before.raw, std::memory_order_relaxed);
    g_typeargs_vector_before.store(vector_before.raw, std::memory_order_relaxed);
    g_typeargs_element_before.store(element_before.raw, std::memory_order_relaxed);
    if (!before_ok) {
        g_typeargs_failures.fetch_add(1, std::memory_order_relaxed);
        __android_log_print(
            ANDROID_LOG_ERROR, "DartPlantModule",
            "DARTPLANT_HOST {event=typeargs_before, state=fail, descriptor=%d, "
            "type_args=%u, named=%u, parameter=%d/%u, vector=%d/%u, element=%d/%u, error=%s}",
            descriptor_status, descriptor.type_args_len, descriptor.named_count, parameter_status,
            static_cast<unsigned>(parameter_before.kind), vector_status,
            static_cast<unsigned>(vector_before.kind), element_status,
            static_cast<unsigned>(element_before.kind), dartplant_last_error());
        return;
    }

    const bool require_relocation = g_typeargs_require_relocation.load(std::memory_order_acquire);
    DartPlantValue parameter_after = parameter_before;
    DartPlantValue vector_after = vector_before;
    DartPlantValue element_after = element_before;
    bool api_ok = true;
    bool parameter_relocated = false;
    bool vector_relocated = false;
    bool element_relocated = false;
    uint64_t allocation_count = 0;
    if (require_relocation) {
        const Dart_Port pressure_port = g_typeargs_pressure_port.load(std::memory_order_acquire);
        if (pressure_port == ILLEGAL_PORT || Dart_NewSendPort_DL == nullptr ||
            Dart_EnterScope_DL == nullptr || Dart_ExitScope_DL == nullptr ||
            Dart_IsError_DL == nullptr) {
            g_typeargs_failures.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        constexpr uint32_t kAllocationsPerBatch = 16384;
        constexpr uint32_t kMaxAllocationBatches = 128;
        for (uint32_t batch = 0; batch < kMaxAllocationBatches; ++batch) {
            Dart_EnterScope_DL();
            bool batch_ok = true;
            for (uint32_t index = 0; index < kAllocationsPerBatch; ++index) {
                Dart_Handle allocation = Dart_NewSendPort_DL(pressure_port);
                ++allocation_count;
                if (allocation == nullptr || Dart_IsError_DL(allocation)) {
                    batch_ok = false;
                    break;
                }
            }
            Dart_ExitScope_DL();
            g_typeargs_gc_calls.store(allocation_count, std::memory_order_relaxed);
            if (!batch_ok) {
                api_ok = false;
                break;
            }
            const DartPlantStatus parameter_after_status =
                dartplant_invocation_get_argument(invocation, 0, &parameter_after);
            const DartPlantStatus vector_after_status =
                dartplant_invocation_get_closure_type_arguments(invocation, &vector_after);
            const DartPlantStatus element_after_status =
                dartplant_invocation_get_closure_type_argument(invocation, 0, &element_after);
            const bool after_ok = parameter_after_status == DARTPLANT_OK &&
                                  parameter_after.kind == DARTPLANT_VALUE_HEAP_OBJECT &&
                                  parameter_after.raw != 0 && vector_after_status == DARTPLANT_OK &&
                                  vector_after.kind == DARTPLANT_VALUE_HEAP_OBJECT &&
                                  vector_after.raw != 0 && element_after_status == DARTPLANT_OK &&
                                  element_after.kind == DARTPLANT_VALUE_HEAP_OBJECT &&
                                  element_after.raw != 0;
            if (!after_ok) {
                api_ok = false;
                break;
            }
            parameter_relocated = parameter_after.raw != parameter_before.raw;
            vector_relocated = vector_after.raw != vector_before.raw;
            element_relocated = element_after.raw != element_before.raw;
            if (parameter_relocated) break;
        }
    } else {
        const DartPlantStatus parameter_after_status =
            dartplant_invocation_get_argument(invocation, 0, &parameter_after);
        const DartPlantStatus vector_after_status =
            dartplant_invocation_get_closure_type_arguments(invocation, &vector_after);
        const DartPlantStatus element_after_status =
            dartplant_invocation_get_closure_type_argument(invocation, 0, &element_after);
        api_ok = parameter_after_status == DARTPLANT_OK &&
                 parameter_after.kind == DARTPLANT_VALUE_HEAP_OBJECT && parameter_after.raw != 0 &&
                 vector_after_status == DARTPLANT_OK &&
                 vector_after.kind == DARTPLANT_VALUE_HEAP_OBJECT && vector_after.raw != 0 &&
                 element_after_status == DARTPLANT_OK &&
                 element_after.kind == DARTPLANT_VALUE_HEAP_OBJECT && element_after.raw != 0;
    }

    g_typeargs_parameter_after.store(parameter_after.raw, std::memory_order_relaxed);
    g_typeargs_vector_after.store(vector_after.raw, std::memory_order_relaxed);
    g_typeargs_element_after.store(element_after.raw, std::memory_order_relaxed);
    g_typeargs_parameter_relocated.store(parameter_relocated, std::memory_order_release);
    g_typeargs_vector_relocated.store(vector_relocated, std::memory_order_release);
    g_typeargs_element_relocated.store(element_relocated, std::memory_order_release);
    const bool passed = api_ok && (!require_relocation || parameter_relocated);
    g_typeargs_callback_passed.store(passed, std::memory_order_release);
    if (!passed) g_typeargs_failures.fetch_add(1, std::memory_order_relaxed);
    __android_log_print(passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=typeargs_callback, state=%s, require_relocation=%u, "
                        "allocations=%llu, parameter_relocated=%u, vector_relocated=%u, "
                        "element_relocated=%u, type_args=1, named=2}",
                        passed ? "pass" : "fail", static_cast<unsigned>(require_relocation),
                        static_cast<unsigned long long>(allocation_count),
                        static_cast<unsigned>(parameter_relocated),
                        static_cast<unsigned>(vector_relocated),
                        static_cast<unsigned>(element_relocated));
}

bool TypeArgsProbe() {
    const uint64_t enter = g_typeargs_enter.load(std::memory_order_acquire);
    const uint64_t failures = g_typeargs_failures.load(std::memory_order_acquire);
    const uint64_t calls = g_typeargs_gc_calls.load(std::memory_order_acquire);
    const bool require_relocation = g_typeargs_require_relocation.load(std::memory_order_acquire);
    const bool passed =
        enter == 1 && failures == 0 &&
        g_typeargs_parameter_before.load(std::memory_order_acquire) != 0 &&
        g_typeargs_parameter_after.load(std::memory_order_acquire) != 0 &&
        g_typeargs_vector_before.load(std::memory_order_acquire) != 0 &&
        g_typeargs_vector_after.load(std::memory_order_acquire) != 0 &&
        g_typeargs_element_before.load(std::memory_order_acquire) != 0 &&
        g_typeargs_element_after.load(std::memory_order_acquire) != 0 &&
        g_typeargs_callback_passed.load(std::memory_order_acquire) &&
        (!require_relocation ||
         (calls != 0 && g_typeargs_parameter_relocated.load(std::memory_order_acquire)));
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=typeargs_probe, state=%s, enter=%llu, failures=%llu, "
        "require_relocation=%u, allocations=%llu, parameter_relocated=%u, "
        "vector_relocated=%u, element_relocated=%u}",
        passed ? "pass" : "fail", static_cast<unsigned long long>(enter),
        static_cast<unsigned long long>(failures), static_cast<unsigned>(require_relocation),
        static_cast<unsigned long long>(calls),
        static_cast<unsigned>(g_typeargs_parameter_relocated.load(std::memory_order_acquire)),
        static_cast<unsigned>(g_typeargs_vector_relocated.load(std::memory_order_acquire)),
        static_cast<unsigned>(g_typeargs_element_relocated.load(std::memory_order_acquire)));
    return passed;
}

void OnExternalClosureEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue closure{};
    DartPlantValue left{};
    DartPlantValue right{};
    DartPlantArgumentsDescriptorInfo descriptor{};
    descriptor.struct_size = sizeof(descriptor);
    uint64_t raw_x0 = 0;
    const bool valid =
        dartplant_invocation_has_verified_abi(invocation) != 0 &&
        dartplant_invocation_argument_count(invocation) == 2 &&
        dartplant_invocation_get_argument(invocation, 0, &left) == DARTPLANT_OK &&
        dartplant_invocation_get_argument(invocation, 1, &right) == DARTPLANT_OK &&
        left.kind == DARTPLANT_VALUE_SMI && right.kind == DARTPLANT_VALUE_SMI &&
        dartplant_invocation_has_closure_receiver(invocation) != 0 &&
        dartplant_invocation_get_closure_receiver(invocation, &closure) == DARTPLANT_OK &&
        closure.kind == DARTPLANT_VALUE_HEAP_OBJECT && closure.raw != 0 &&
        dartplant_invocation_get_gp_register(invocation, 0, &raw_x0) == DARTPLANT_OK &&
        closure.raw == raw_x0 &&
        dartplant_invocation_get_arguments_descriptor(invocation, &descriptor) == DARTPLANT_OK &&
        descriptor.type_args_len == 0 && descriptor.count == 3 && descriptor.size == 3 &&
        descriptor.positional_count == 3 && descriptor.named_count == 0;
    if (!valid) {
        g_closure_failures.fetch_add(1, std::memory_order_relaxed);
        __android_log_print(
            ANDROID_LOG_ERROR, "DartPlantModule",
            "DARTPLANT_HOST {event=closure_callback, state=fail, verified=%u, argc=%u, "
            "left_kind=%u, right_kind=%u, closure_kind=%u, x0_match=%u, "
            "type_args=%u, count=%u, size=%u, positional=%u, named=%u, error=%s}",
            static_cast<unsigned>(dartplant_invocation_has_verified_abi(invocation)),
            dartplant_invocation_argument_count(invocation), static_cast<unsigned>(left.kind),
            static_cast<unsigned>(right.kind), static_cast<unsigned>(closure.kind),
            static_cast<unsigned>(closure.raw != 0 && closure.raw == raw_x0),
            descriptor.type_args_len, descriptor.count, descriptor.size,
            descriptor.positional_count, descriptor.named_count, dartplant_last_error());
        return;
    }
    g_closure_enter.fetch_add(1, std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_INFO, "DartPlantModule",
                        "DARTPLANT_HOST {event=closure_callback, state=pass, receiver_x0=1, "
                        "typed_stack=1, args_desc=1}");
}

bool ClosureProbe() {
    const uint32_t enter = g_closure_enter.load(std::memory_order_acquire);
    const uint32_t failures = g_closure_failures.load(std::memory_order_acquire);
    const bool active =
        g_closure_hook != nullptr && g_closure_hook->active.load(std::memory_order_acquire);
    const bool passed = enter == 1 && failures == 0 && active;
    __android_log_print(
        passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=closure_probe, state=%s, enter=%u, failures=%u, active=%u}",
        passed ? "pass" : "fail", enter, failures, static_cast<unsigned>(active));
    return passed;
}

void OnExternalOrdinaryEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue left{};
    DartPlantValue right{};
    const bool valid = dartplant_invocation_has_verified_abi(invocation) != 0 &&
                       dartplant_invocation_argument_count(invocation) == 2 &&
                       dartplant_invocation_get_argument(invocation, 0, &left) == DARTPLANT_OK &&
                       dartplant_invocation_get_argument(invocation, 1, &right) == DARTPLANT_OK &&
                       left.kind == DARTPLANT_VALUE_DOUBLE && right.kind == DARTPLANT_VALUE_DOUBLE;
    if (!valid) {
        g_ordinary_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    left.raw = std::bit_cast<uint64_t>(std::bit_cast<double>(left.raw) + 1.0);
    if (dartplant_invocation_set_argument(invocation, 0, &left) != DARTPLANT_OK) {
        g_ordinary_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ordinary_enter.fetch_add(1, std::memory_order_relaxed);
}

void OnExternalOrdinaryLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_DOUBLE) {
        g_ordinary_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    result.raw = std::bit_cast<uint64_t>(std::bit_cast<double>(result.raw) + 10.0);
    if (dartplant_invocation_set_result(invocation, &result) != DARTPLANT_OK) {
        g_ordinary_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ordinary_leave.fetch_add(1, std::memory_order_relaxed);
}

void OnExternalOrdinaryObserver(DartPlantInvocation* invocation, void*) {
    if (dartplant_invocation_has_verified_abi(invocation) == 0) {
        g_ordinary_failures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_ordinary_observer_enter.fetch_add(1, std::memory_order_relaxed);
}

bool OrdinaryProbeAndCleanup() {
    const uint32_t enter = g_ordinary_enter.load(std::memory_order_acquire);
    const uint32_t leave = g_ordinary_leave.load(std::memory_order_acquire);
    const uint32_t observer = g_ordinary_observer_enter.load(std::memory_order_acquire);
    const uint32_t failures = g_ordinary_failures.load(std::memory_order_acquire);
    bool cleanup = true;
    if (g_ordinary_observer != nullptr) {
        cleanup = dartplant_unhook_handle(g_ordinary_observer) == DARTPLANT_OK &&
                  dartplant_hook_handle_is_idle(g_ordinary_observer) != 0 && cleanup;
        if (cleanup) {
            dartplant_release_hook_handle(g_ordinary_observer);
            g_ordinary_observer = nullptr;
        }
    }
    if (g_ordinary_hook != nullptr) {
        const bool primary_cleanup = dartplant_unhook_handle(g_ordinary_hook) == DARTPLANT_OK &&
                                     dartplant_hook_handle_is_idle(g_ordinary_hook) != 0;
        cleanup = cleanup && primary_cleanup;
        if (primary_cleanup) {
            dartplant_release_hook_handle(g_ordinary_hook);
            g_ordinary_hook = nullptr;
        }
    }
    if (g_ordinary_method != nullptr && cleanup) {
        dartplant_release_method(g_ordinary_method);
        g_ordinary_method = nullptr;
    }
    const bool passed = enter == 1 && leave == 1 && observer == 1 && failures == 0 && cleanup;
    __android_log_print(passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=ordinary_probe, state=%s, enter=%u, leave=%u, "
                        "observer=%u, failures=%u, cleanup=%u}",
                        passed ? "pass" : "fail", enter, leave, observer, failures,
                        static_cast<unsigned>(cleanup));
    return passed;
}

struct ExternalP6State {
    DartPlantHookHandle* int64_handle = nullptr;
    DartPlantHookHandle* entry_stack_handle = nullptr;
    DartPlantHookHandle* odd_stack_handle = nullptr;
    DartPlantHookHandle* throwing_stack_handle = nullptr;
    DartPlantHookHandle* forced_stack_handle = nullptr;
    DartPlantHookHandle* pair_handle = nullptr;
    std::atomic_uint32_t int64_enter{0};
    std::atomic_uint32_t int64_leave{0};
    std::atomic_uint32_t entry_stack_enter{0};
    std::atomic_uint32_t entry_stack_leave{0};
    std::atomic_uint32_t odd_stack_enter{0};
    std::atomic_uint32_t odd_stack_leave{0};
    std::atomic_uint32_t throwing_stack_enter{0};
    std::atomic_uint32_t throwing_stack_leave{0};
    std::atomic_uint32_t throwing_stack_exception{0};
    std::atomic_uint32_t exception_object_observed{0};
    std::atomic_uint32_t forced_stack_enter{0};
    std::atomic_uint32_t forced_stack_leave{0};
    std::atomic_uint32_t pair_leave{0};
    std::atomic_uint32_t moving_gc_pair_leave{0};
    std::atomic_uint32_t failures{0};
};

ExternalP6State g_p6;

void P6Fail(const char* stage) {
    g_p6.failures.fetch_add(1, std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=p6_callback, state=fail, stage=%s, error=%s}", stage,
                        dartplant_last_error());
}

bool RemoveP6Handle(DartPlantHookHandle** handle) {
    if (handle == nullptr || *handle == nullptr) return true;
    if (dartplant_unhook_handle(*handle) != DARTPLANT_OK ||
        dartplant_hook_handle_is_idle(*handle) == 0) {
        return false;
    }
    dartplant_release_hook_handle(*handle);
    *handle = nullptr;
    return true;
}

void P6Int64Enter(DartPlantInvocation* invocation, void*) {
    DartPlantValue left{};
    DartPlantValue right{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 2 ||
        dartplant_invocation_get_argument(invocation, 0, &left) != DARTPLANT_OK ||
        dartplant_invocation_get_argument(invocation, 1, &right) != DARTPLANT_OK ||
        left.kind != DARTPLANT_VALUE_INT64 || right.kind != DARTPLANT_VALUE_INT64) {
        P6Fail("int64_enter_decode");
        return;
    }
    left.raw = std::bit_cast<uint64_t>(std::bit_cast<int64_t>(left.raw) + 1);
    if (dartplant_invocation_set_argument(invocation, 0, &left) != DARTPLANT_OK ||
        dartplant_invocation_call_original(invocation) != DARTPLANT_OK) {
        P6Fail("int64_enter_rewrite");
        return;
    }
    g_p6.int64_enter.fetch_add(1, std::memory_order_relaxed);
}

void P6Int64Leave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_INT64) {
        P6Fail("int64_leave_decode");
        return;
    }
    result.raw = std::bit_cast<uint64_t>(std::bit_cast<int64_t>(result.raw) + 100);
    if (dartplant_invocation_set_result(invocation, &result) != DARTPLANT_OK) {
        P6Fail("int64_leave_rewrite");
        return;
    }
    g_p6.int64_leave.fetch_add(1, std::memory_order_relaxed);
}

void P6EntryStackEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue a6{};
    DartPlantValue a7{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 8 ||
        dartplant_invocation_get_argument(invocation, 6, &a6) != DARTPLANT_OK ||
        dartplant_invocation_get_argument(invocation, 7, &a7) != DARTPLANT_OK ||
        a6.kind != DARTPLANT_VALUE_DOUBLE || a7.kind != DARTPLANT_VALUE_DOUBLE) {
        P6Fail("entry_stack_decode");
        return;
    }
    a6.raw = std::bit_cast<uint64_t>(std::bit_cast<double>(a6.raw) + 1.0);
    a7.raw = std::bit_cast<uint64_t>(std::bit_cast<double>(a7.raw) + 2.0);
    if (dartplant_invocation_set_argument(invocation, 6, &a6) != DARTPLANT_OK ||
        dartplant_invocation_set_argument(invocation, 7, &a7) != DARTPLANT_OK) {
        P6Fail("entry_stack_rewrite");
        return;
    }
    g_p6.entry_stack_enter.fetch_add(1, std::memory_order_relaxed);
}

void P6EntryStackLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_DOUBLE) {
        P6Fail("entry_stack_leave_decode");
        return;
    }
    result.raw = std::bit_cast<uint64_t>(std::bit_cast<double>(result.raw) + 1000.0);
    if (dartplant_invocation_set_result(invocation, &result) != DARTPLANT_OK) {
        P6Fail("entry_stack_leave_rewrite");
        return;
    }
    g_p6.entry_stack_leave.fetch_add(1, std::memory_order_relaxed);
}

void P6OddStackEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue a6{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 7 ||
        dartplant_invocation_get_argument(invocation, 6, &a6) != DARTPLANT_OK ||
        a6.kind != DARTPLANT_VALUE_DOUBLE) {
        P6Fail("odd_stack_decode");
        return;
    }
    a6.raw = std::bit_cast<uint64_t>(std::bit_cast<double>(a6.raw) + 1.0);
    if (dartplant_invocation_set_argument(invocation, 6, &a6) != DARTPLANT_OK) {
        P6Fail("odd_stack_rewrite");
        return;
    }
    g_p6.odd_stack_enter.fetch_add(1, std::memory_order_relaxed);
}

void P6OddStackLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_DOUBLE) {
        P6Fail("odd_stack_leave_decode");
        return;
    }
    result.raw = std::bit_cast<uint64_t>(std::bit_cast<double>(result.raw) + 100.0);
    if (dartplant_invocation_set_result(invocation, &result) != DARTPLANT_OK) {
        P6Fail("odd_stack_leave_rewrite");
        return;
    }
    g_p6.odd_stack_leave.fetch_add(1, std::memory_order_relaxed);
}

void P6ThrowEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue first{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 8 ||
        dartplant_invocation_get_argument(invocation, 0, &first) != DARTPLANT_OK ||
        first.kind != DARTPLANT_VALUE_DOUBLE) {
        P6Fail("throw_enter_decode");
        return;
    }
    g_p6.throwing_stack_enter.fetch_add(1, std::memory_order_relaxed);
}

void P6ThrowLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_DOUBLE) {
        P6Fail("throw_leave_decode");
        return;
    }
    g_p6.throwing_stack_leave.fetch_add(1, std::memory_order_relaxed);
}

void P6ThrowException(DartPlantInvocation* invocation, void*) {
    DartPlantValue exception{};
    DartPlantValue stacktrace{};
    DartPlantValue argument{};
    uint64_t gp = 0;
    const bool valid =
        dartplant_invocation_phase(invocation) == DARTPLANT_INVOCATION_EXCEPTION &&
        dartplant_invocation_get_exception(invocation, &exception) == DARTPLANT_OK &&
        dartplant_invocation_get_stacktrace(invocation, &stacktrace) == DARTPLANT_OK &&
        exception.kind == DARTPLANT_VALUE_HEAP_OBJECT &&
        stacktrace.kind == DARTPLANT_VALUE_HEAP_OBJECT &&
        dartplant_invocation_get_argument(invocation, 0, &argument) ==
            DARTPLANT_INVALID_INVOCATION_PHASE &&
        dartplant_invocation_get_gp_register(invocation, 0, &gp) ==
            DARTPLANT_INVALID_INVOCATION_PHASE;
    if (!valid) {
        P6Fail("throw_exception_bridge");
        return;
    }
    g_p6.exception_object_observed.fetch_add(1, std::memory_order_relaxed);
    g_p6.throwing_stack_exception.fetch_add(1, std::memory_order_relaxed);
}

void P6ForcedEnter(DartPlantInvocation* invocation, void*) {
    DartPlantValue left{};
    DartPlantValue right{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_argument_count(invocation) != 2 ||
        dartplant_invocation_get_argument(invocation, 0, &left) != DARTPLANT_OK ||
        dartplant_invocation_get_argument(invocation, 1, &right) != DARTPLANT_OK ||
        left.kind != DARTPLANT_VALUE_SMI || right.kind != DARTPLANT_VALUE_SMI ||
        dartplant_invocation_set_argument(invocation, 0, &right) != DARTPLANT_OK ||
        dartplant_invocation_set_argument(invocation, 1, &left) != DARTPLANT_OK) {
        P6Fail("forced_stack");
        return;
    }
    g_p6.forced_stack_enter.fetch_add(1, std::memory_order_relaxed);
}

void P6ForcedLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValue result{};
    if (dartplant_invocation_get_result(invocation, &result) != DARTPLANT_OK ||
        result.kind != DARTPLANT_VALUE_SMI) {
        P6Fail("forced_stack_leave");
        return;
    }
    g_p6.forced_stack_leave.fetch_add(1, std::memory_order_relaxed);
}

void P6PairLeave(DartPlantInvocation* invocation, void*) {
    DartPlantValuePair pair{};
    if (dartplant_invocation_has_verified_abi(invocation) == 0 ||
        dartplant_invocation_get_result_pair(invocation, &pair) != DARTPLANT_OK) {
        P6Fail("pair_decode");
        return;
    }
    const bool smi =
        pair.first.kind == DARTPLANT_VALUE_SMI && pair.second.kind == DARTPLANT_VALUE_SMI;
    const bool object = pair.first.kind == DARTPLANT_VALUE_HEAP_OBJECT &&
                        pair.second.kind == DARTPLANT_VALUE_HEAP_OBJECT;
    if (!smi && !object) {
        P6Fail("pair_kind");
        return;
    }
    if (object) {
        usleep(500000);
        if (dartplant_invocation_get_result_pair(invocation, &pair) != DARTPLANT_OK ||
            pair.first.kind != DARTPLANT_VALUE_HEAP_OBJECT ||
            pair.second.kind != DARTPLANT_VALUE_HEAP_OBJECT) {
            P6Fail("pair_gc_refresh");
            return;
        }
        g_p6.moving_gc_pair_leave.fetch_add(1, std::memory_order_relaxed);
    }
    const DartPlantValuePair swapped = {.first = pair.second, .second = pair.first};
    if (dartplant_invocation_set_result_pair(invocation, &swapped) != DARTPLANT_OK) {
        P6Fail("pair_swap");
        return;
    }
    g_p6.pair_leave.fetch_add(1, std::memory_order_relaxed);
}

DartPlantStatus InstallP6One(const char* function_name,
                             const DartPlantCompilerAbiEvidence* evidence,
                             DartPlantInvocationCallback on_enter,
                             DartPlantInvocationCallback on_leave, DartPlantHookHandle** out_handle,
                             bool use_vm_adapter) {
    DartPlantRuntime* runtime = dartplant::DefaultRuntimeInstanceForTesting();
    if (runtime == nullptr) return DARTPLANT_RUNTIME_NOT_READY;
    const DartPlantMethodQuery query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "Global",
        .function_name = function_name,
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    DartPlantStatus status = dartplant_runtime_find_method(runtime, &query, &method);
    if (status != DARTPLANT_OK) return status;
    status = dartplant_runtime_register_compiler_abi_evidence(runtime, method, evidence);
    if (status == DARTPLANT_OK) {
        DartPlantMethodAbiInfo info{};
        info.struct_size = sizeof(info);
        status = dartplant_runtime_get_method_abi_info(runtime, method, &info);
        if (status == DARTPLANT_OK &&
            (info.state != DARTPLANT_METHOD_ABI_VERIFIED || info.has_verified_call_layout == 0)) {
            status = DARTPLANT_UNSUPPORTED_ABI;
        }
    }
    if (status == DARTPLANT_OK) {
        DartPlantHookOptions options{};
        options.struct_size = sizeof(options);
        options.on_enter = on_enter;
        options.on_leave = on_leave;
        options.vm_adapter =
            use_vm_adapter ? dartplant_flutter_vm_adapter_get(g_vm_adapter) : nullptr;
        status = dartplant_runtime_hook_method_handle(runtime, method, &options, out_handle);
    }
    dartplant_release_method(method);
    return status;
}

void ResetP6State() {
    g_p6.int64_enter.store(0, std::memory_order_relaxed);
    g_p6.int64_leave.store(0, std::memory_order_relaxed);
    g_p6.entry_stack_enter.store(0, std::memory_order_relaxed);
    g_p6.entry_stack_leave.store(0, std::memory_order_relaxed);
    g_p6.odd_stack_enter.store(0, std::memory_order_relaxed);
    g_p6.odd_stack_leave.store(0, std::memory_order_relaxed);
    g_p6.throwing_stack_enter.store(0, std::memory_order_relaxed);
    g_p6.throwing_stack_leave.store(0, std::memory_order_relaxed);
    g_p6.throwing_stack_exception.store(0, std::memory_order_relaxed);
    g_p6.exception_object_observed.store(0, std::memory_order_relaxed);
    g_p6.forced_stack_enter.store(0, std::memory_order_relaxed);
    g_p6.forced_stack_leave.store(0, std::memory_order_relaxed);
    g_p6.pair_leave.store(0, std::memory_order_relaxed);
    g_p6.moving_gc_pair_leave.store(0, std::memory_order_relaxed);
    g_p6.failures.store(0, std::memory_order_relaxed);
}

uint64_t Fail(const char* stage, DartPlantStatus status) {
    __android_log_print(ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=owner_entry, state=fail, stage=%s, status=%d, "
                        "error=%s}",
                        stage, static_cast<int>(status), dartplant_last_error());
    return 0;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeStatus(JNIEnv*, jclass) {
    const uint32_t descriptors = dartplant_flutter_vm_descriptor_count();
    const bool host_ready = dartplant_lsposed_native_host_ready() != 0;
    const bool native_v2_proved = host_ready && dartplant_lsposed_native_backend_selftest() != 0;
    const bool ready =
        (native_v2_proved || g_guest_backend_prepared.load(std::memory_order_acquire)) &&
        descriptors == 2;
    __android_log_print(ready ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=native_status, ready=%u, descriptors=%u, "
                        "native_v2_present=%u, native_v2_selftest=%u}",
                        static_cast<unsigned>(ready), descriptors,
                        static_cast<unsigned>(host_ready), static_cast<unsigned>(native_v2_proved));
    return ready ? 1 : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeInitTranslatedDobby(JNIEnv*, jclass) {
    if (dartplant_is_initialized() != 0) return 1;
    // Prepare the physical backend only: Flutter's libapp/libflutter mappings
    // are not loaded yet at onModuleLoaded. Runtime creation belongs in the
    // actual owner-thread FFI handshake, after those mappings exist.
    const DartPlantStatus status = dartplant_install_host_api(dartplant_dobby_host_api());
    if (status == DARTPLANT_OK) {
        g_guest_backend_prepared.store(true, std::memory_order_release);
    }
    __android_log_print(
        status == DARTPLANT_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=translated_backend, mode=arm64_strict_dobby, "
        "status=%d, error=%s}",
        static_cast<int>(status), status == DARTPLANT_OK ? "none" : dartplant_last_error());
    return status == DARTPLANT_OK ? 1 : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeInitNativeStrictDobby(JNIEnv*, jclass) {
    // A separately opted-in native ARM64 test configuration. The API v2
    // preflight has already rejected the framework backend. We explicitly
    // substitute our local strict publication backend without claiming the
    // framework's own hook/unhook pair is functional.
    const DartPlantStatus status = dartplant_install_host_api(dartplant_dobby_host_api());
    if (status == DARTPLANT_OK) {
        g_guest_backend_prepared.store(true, std::memory_order_release);
    }
    __android_log_print(
        status == DARTPLANT_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=native_strict_backend, mode=arm64_strict_dobby, "
        "native_v2_selftest=fail, status=%d, error=%s}",
        static_cast<int>(status), status == DARTPLANT_OK ? "none" : dartplant_last_error());
    return status == DARTPLANT_OK ? 1 : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeBootstrapEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_bootstrap);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeCountsEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_counts);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeRetireEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_retire);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeMappingEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_physical_mapping_control);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeExceptionEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_exception_probe);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeObjectRootEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_object_root_probe);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeLoaderDrainEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_loader_callback_retire);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeTypeArgsPrepareEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_typeargs_prepare);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeTypeArgsProbeEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_typeargs_probe);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeP6InstallEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_p6_install);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeP6ProbeEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_p6_probe);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeClosureInstallEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_closure_install);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeClosureProbeEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_closure_probe);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeOrdinaryInstallEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_ordinary_install);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeOrdinaryMarkSharedEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_ordinary_mark_shared);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_dartplant_integration_DartPlantModule_nativeOrdinaryProbeEntry(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(&dartplant_module_external_ordinary_probe);
}

extern "C" __attribute__((visibility("default"))) uint64_t dartplant_module_external_counts() {
    return (static_cast<uint64_t>(g_enter.load(std::memory_order_acquire)) << 32) |
           g_leave.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) uint64_t
dartplant_module_external_semantic_probe() {
    return SemanticProbe() ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) uint64_t
dartplant_module_external_exception_probe() {
    return ExceptionProbe() ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) uint64_t
dartplant_module_external_object_root_probe() {
    return (ObjectRootProbe() ? 1ULL : 0ULL) |
           (g_root_relocated.load(std::memory_order_acquire) ? 2ULL : 0ULL);
}

extern "C" __attribute__((visibility("default"))) int32_t
dartplant_module_external_typeargs_prepare(Dart_Handle retained_closure,
                                           Dart_Handle pressure_send_port,
                                           uint8_t require_relocation) {
    std::lock_guard lock(g_bootstrap_mutex);
    if (g_vm_adapter == nullptr || retained_closure == nullptr || pressure_send_port == nullptr) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    ResetTypeArgsProbe();
    g_typeargs_require_relocation.store(require_relocation != 0, std::memory_order_release);
    if (Dart_SendPortGetId_DL == nullptr || Dart_IsError_DL == nullptr) {
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    Dart_Port port = ILLEGAL_PORT;
    Dart_Handle port_status = Dart_SendPortGetId_DL(pressure_send_port, &port);
    if (port_status == nullptr || Dart_IsError_DL(port_status) || port == ILLEGAL_PORT) {
        return DARTPLANT_INVALID_ARGUMENT;
    }
    g_typeargs_pressure_port.store(port, std::memory_order_release);
    if (g_typeargs_hook != nullptr && g_typeargs_hook->active.load(std::memory_order_acquire) &&
        g_typeargs_method != nullptr) {
        return DARTPLANT_OK;
    }
    if (g_typeargs_hook != nullptr) {
        if (g_typeargs_hook->active.load(std::memory_order_acquire)) {
            (void) dartplant_unhook(g_typeargs_hook);
        }
        dartplant_release_hook(g_typeargs_hook);
        g_typeargs_hook = nullptr;
    }
    if (g_typeargs_method != nullptr) {
        dartplant_release_method(g_typeargs_method);
        g_typeargs_method = nullptr;
    }

    DartPlantRuntime* runtime = dartplant::DefaultRuntimeInstanceForTesting();
    if (runtime == nullptr) return DARTPLANT_RUNTIME_NOT_READY;
    const DartPlantMethodQuery query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "Global",
        .function_name = "[tear-off] signatureProbe",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantStatus status = dartplant_runtime_find_method(runtime, &query, &g_typeargs_method);
    if (status != DARTPLANT_OK) return status;
    status = dartplant_runtime_register_compiler_abi_evidence(
        runtime, g_typeargs_method, &kDartPlantTypeArgumentsClosureAbiEvidence);
    if (status != DARTPLANT_OK) return status;
    DartPlantMethodAbiInfo abi_info{};
    abi_info.struct_size = sizeof(abi_info);
    status = dartplant_runtime_get_method_abi_info(runtime, g_typeargs_method, &abi_info);
    if (status != DARTPLANT_OK || abi_info.state != DARTPLANT_METHOD_ABI_VERIFIED ||
        abi_info.has_verified_call_layout == 0) {
        return status != DARTPLANT_OK ? status : DARTPLANT_UNSUPPORTED_ABI;
    }
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    DartPlantHookOptions options{};
    options.struct_size = sizeof(options);
    options.on_enter = OnTypeArgsEnter;
    options.vm_adapter = dartplant_flutter_vm_adapter_get(g_vm_adapter);
    if (options.vm_adapter == nullptr) return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    status = dartplant_runtime_hook_method_with_profile(runtime, g_typeargs_method, &profile,
                                                        &options, &g_typeargs_hook);
    __android_log_print(
        status == DARTPLANT_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=typeargs_prepare, state=%s, require_relocation=%u, "
        "verified_layout=%u, params=%u, stack=%u, retained_closure=1, status=%d}",
        status == DARTPLANT_OK ? "pass" : "fail", static_cast<unsigned>(require_relocation != 0),
        static_cast<unsigned>(abi_info.has_verified_call_layout), abi_info.parameter_count,
        abi_info.stack_words, status);
    return status;
}

extern "C" __attribute__((visibility("default"))) uint64_t
dartplant_module_external_typeargs_probe() {
    const bool type_args = TypeArgsProbe();
    const bool transition = TransitionProbe();
    return type_args && transition ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) int32_t
dartplant_module_external_closure_install() {
    std::lock_guard lock(g_bootstrap_mutex);
    if (g_vm_adapter == nullptr) return DARTPLANT_RUNTIME_NOT_READY;
    if (g_closure_hook != nullptr || g_closure_method != nullptr) {
        return DARTPLANT_ALREADY_HOOKED;
    }
    g_closure_enter.store(0, std::memory_order_relaxed);
    g_closure_failures.store(0, std::memory_order_relaxed);
    const DartPlantInitInfo artifact_init = {
        .struct_size = sizeof(DartPlantInitInfo),
        .version = DARTPLANT_INIT_API_VERSION,
        .host_api = nullptr,
        .artifact_bundle = &kDartPlantP6ForcedStackClosureArtifactBundle,
        .app_module_name = nullptr,
        .runtime_module_name = nullptr,
    };
    DartPlantStatus status = dartplant_init(&artifact_init);
    if (status != DARTPLANT_OK) return status;
    DartPlantRuntime* runtime = dartplant::DefaultRuntimeInstanceForTesting();
    if (runtime == nullptr) return DARTPLANT_RUNTIME_NOT_READY;
    const DartPlantMethodQuery query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "Global",
        .function_name = "[tear-off] verifiedAbiForcedStack",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    status = dartplant_runtime_find_method(runtime, &query, &g_closure_method);
    if (status != DARTPLANT_OK) return status;
    status = dartplant_runtime_register_compiler_abi_evidence(
        runtime, g_closure_method, &kDartPlantP6ForcedStackClosureAbiEvidence);
    if (status != DARTPLANT_OK) return status;
    DartPlantMethodAbiInfo info{};
    info.struct_size = sizeof(info);
    status = dartplant_runtime_get_method_abi_info(runtime, g_closure_method, &info);
    if (status != DARTPLANT_OK || info.state != DARTPLANT_METHOD_ABI_VERIFIED ||
        info.has_verified_call_layout == 0 || info.parameter_count != 2 || info.stack_words != 2) {
        return status != DARTPLANT_OK ? status : DARTPLANT_UNSUPPORTED_ABI;
    }
    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    DartPlantHookOptions options{};
    options.struct_size = sizeof(options);
    options.on_enter = OnExternalClosureEnter;
    status = dartplant_runtime_hook_method_with_profile(runtime, g_closure_method, &profile,
                                                        &options, &g_closure_hook);
    __android_log_print(status == DARTPLANT_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                        "DartPlantModule",
                        "DARTPLANT_HOST {event=closure_install, state=%s, status=%d, "
                        "verified_layout=%u, params=%u, stack=%u}",
                        status == DARTPLANT_OK ? "pass" : "fail", status,
                        static_cast<unsigned>(info.has_verified_call_layout), info.parameter_count,
                        info.stack_words);
    return status;
}

extern "C" __attribute__((visibility("default"))) uint64_t
dartplant_module_external_closure_probe() {
    return ClosureProbe() ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) int32_t
dartplant_module_external_ordinary_install() {
    std::lock_guard lock(g_bootstrap_mutex);
    if (g_vm_adapter == nullptr) return DARTPLANT_RUNTIME_NOT_READY;
    if (g_ordinary_hook != nullptr || g_ordinary_observer != nullptr ||
        g_ordinary_method != nullptr) {
        return DARTPLANT_ALREADY_HOOKED;
    }
    g_ordinary_enter.store(0, std::memory_order_relaxed);
    g_ordinary_leave.store(0, std::memory_order_relaxed);
    g_ordinary_observer_enter.store(0, std::memory_order_relaxed);
    g_ordinary_failures.store(0, std::memory_order_relaxed);
    DartPlantRuntime* runtime = dartplant::DefaultRuntimeInstanceForTesting();
    if (runtime == nullptr) return DARTPLANT_RUNTIME_NOT_READY;
    const DartPlantMethodQuery query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "Global",
        .function_name = "verifiedAbiDouble",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantStatus status = dartplant_runtime_find_method(runtime, &query, &g_ordinary_method);
    if (status != DARTPLANT_OK) return status;
    status = dartplant_runtime_register_compiler_abi_evidence(runtime, g_ordinary_method,
                                                              &kDartPlantOrdinaryAotAbiEvidence);
    if (status != DARTPLANT_OK) return status;
    DartPlantMethodAbiInfo info{};
    info.struct_size = sizeof(info);
    status = dartplant_runtime_get_method_abi_info(runtime, g_ordinary_method, &info);
    if (status != DARTPLANT_OK || info.state != DARTPLANT_METHOD_ABI_VERIFIED ||
        info.has_verified_call_layout == 0) {
        return status != DARTPLANT_OK ? status : DARTPLANT_UNSUPPORTED_ABI;
    }
    DartPlantHookOptions options{};
    options.struct_size = sizeof(options);
    options.on_enter = OnExternalOrdinaryEnter;
    options.on_leave = OnExternalOrdinaryLeave;
    status = dartplant_runtime_hook_method_handle(runtime, g_ordinary_method, &options,
                                                  &g_ordinary_hook);
    if (status == DARTPLANT_OK) {
        DartPlantHookOptions observer{};
        observer.struct_size = sizeof(observer);
        observer.on_enter = OnExternalOrdinaryObserver;
        status = dartplant_runtime_hook_method_handle(runtime, g_ordinary_method, &observer,
                                                      &g_ordinary_observer);
    }
    const bool ready =
        status == DARTPLANT_OK && g_ordinary_hook != nullptr && g_ordinary_observer != nullptr;
    __android_log_print(ready ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=ordinary_install, state=%s, status=%d, "
                        "verified_layout=%u, params=%u, stack=%u}",
                        ready ? "pass" : "fail", status,
                        static_cast<unsigned>(info.has_verified_call_layout), info.parameter_count,
                        info.stack_words);
    return ready ? DARTPLANT_OK : (status == DARTPLANT_OK ? DARTPLANT_HOOK_FAILED : status);
}

extern "C" __attribute__((visibility("default"))) uint64_t
dartplant_module_external_ordinary_mark_shared() {
    std::lock_guard lock(g_bootstrap_mutex);
    if (g_ordinary_method == nullptr || g_ordinary_method->function == nullptr ||
        g_ordinary_method->function->code_target == nullptr || g_ordinary_hook == nullptr ||
        g_ordinary_observer == nullptr) {
        return 0;
    }
    if (g_ordinary_observer_enter.load(std::memory_order_acquire) != 1 ||
        dartplant_unhook_handle(g_ordinary_observer) != DARTPLANT_OK ||
        dartplant_hook_handle_is_idle(g_ordinary_observer) == 0) {
        return 0;
    }
    dartplant_release_hook_handle(g_ordinary_observer);
    g_ordinary_observer = nullptr;
    if (dartplant_hook_handle_is_active(g_ordinary_hook) == 0) return 0;
    const dartplant::DartMethodIdentity late_alias = {
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "Global",
        .function_name = "verifiedAbiDoubleExternalLateAlias",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    g_ordinary_method->function->code_target->AddAlias(late_alias);
    DartPlantMethodAbiInfo info{};
    info.struct_size = sizeof(info);
    DartPlantRuntime* runtime = dartplant::DefaultRuntimeInstanceForTesting();
    const DartPlantStatus status = runtime == nullptr ? DARTPLANT_RUNTIME_NOT_READY
                                                      : dartplant_runtime_get_method_abi_info(
                                                            runtime, g_ordinary_method, &info);
    const bool passed =
        status == DARTPLANT_OK && g_ordinary_method->function->code_target->IsShared() &&
        info.state == DARTPLANT_METHOD_ABI_UNSUPPORTED && info.has_verified_call_layout == 0 &&
        dartplant_hook_handle_is_active(g_ordinary_hook) != 0;
    __android_log_print(passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=ordinary_mark_shared, state=%s, status=%d, "
                        "abi_state=%u, verified_layout=%u, aliases=%u, primary_active=%u}",
                        passed ? "pass" : "fail", status, static_cast<unsigned>(info.state),
                        static_cast<unsigned>(info.has_verified_call_layout),
                        g_ordinary_method->function->code_target->AliasCount(),
                        static_cast<unsigned>(dartplant_hook_handle_is_active(g_ordinary_hook)));
    return passed ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) uint64_t
dartplant_module_external_ordinary_probe() {
    return OrdinaryProbeAndCleanup() ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) int32_t dartplant_module_external_p6_install() {
    std::lock_guard lock(g_bootstrap_mutex);
    if (g_vm_adapter == nullptr) return DARTPLANT_RUNTIME_NOT_READY;
    if (g_p6.int64_handle != nullptr || g_p6.entry_stack_handle != nullptr ||
        g_p6.odd_stack_handle != nullptr || g_p6.throwing_stack_handle != nullptr ||
        g_p6.forced_stack_handle != nullptr || g_p6.pair_handle != nullptr) {
        return DARTPLANT_ALREADY_HOOKED;
    }
    ResetP6State();
    DartPlantStatus status = InstallP6One("verifiedAbiInt64", &kDartPlantP6Int64AbiEvidence,
                                          P6Int64Enter, P6Int64Leave, &g_p6.int64_handle, false);
    if (status == DARTPLANT_OK) {
        status =
            InstallP6One("verifiedAbiEntryStack", &kDartPlantP6EntryStackAbiEvidence,
                         P6EntryStackEnter, P6EntryStackLeave, &g_p6.entry_stack_handle, false);
    }
    if (status == DARTPLANT_OK) {
        status = InstallP6One("verifiedAbiOddStack", &kDartPlantP6OddStackAbiEvidence,
                              P6OddStackEnter, P6OddStackLeave, &g_p6.odd_stack_handle, false);
    }
    if (status == DARTPLANT_OK) {
        status = InstallP6One("verifiedAbiThrowingStack", &kDartPlantP6ThrowingStackAbiEvidence,
                              P6ThrowEnter, P6ThrowLeave, &g_p6.throwing_stack_handle, true);
        if (status == DARTPLANT_OK) {
            status = dartplant_hook_handle_set_exception_callback(g_p6.throwing_stack_handle,
                                                                  P6ThrowException, nullptr);
        }
    }
    if (status == DARTPLANT_OK) {
        status = InstallP6One("verifiedAbiForcedStack", &kDartPlantP6ForcedStackAbiEvidence,
                              P6ForcedEnter, P6ForcedLeave, &g_p6.forced_stack_handle, false);
    }
    if (status == DARTPLANT_OK) {
        status = InstallP6One("verifiedAbiPair", &kDartPlantP6PairAbiEvidence, nullptr, P6PairLeave,
                              &g_p6.pair_handle, true);
    }
    const bool ready = status == DARTPLANT_OK && g_p6.int64_handle != nullptr &&
                       g_p6.entry_stack_handle != nullptr && g_p6.odd_stack_handle != nullptr &&
                       g_p6.throwing_stack_handle != nullptr &&
                       g_p6.forced_stack_handle != nullptr && g_p6.pair_handle != nullptr;
    __android_log_print(ready ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=p6_install, state=%s, status=%d, "
                        "int64=%u, entry_stack=%u, odd_stack=%u, throw=%u, forced=%u, pair=%u}",
                        ready ? "pass" : "fail", status,
                        static_cast<unsigned>(g_p6.int64_handle != nullptr),
                        static_cast<unsigned>(g_p6.entry_stack_handle != nullptr),
                        static_cast<unsigned>(g_p6.odd_stack_handle != nullptr),
                        static_cast<unsigned>(g_p6.throwing_stack_handle != nullptr),
                        static_cast<unsigned>(g_p6.forced_stack_handle != nullptr),
                        static_cast<unsigned>(g_p6.pair_handle != nullptr));
    if (!ready) {
        (void) RemoveP6Handle(&g_p6.pair_handle);
        (void) RemoveP6Handle(&g_p6.forced_stack_handle);
        (void) RemoveP6Handle(&g_p6.throwing_stack_handle);
        (void) RemoveP6Handle(&g_p6.odd_stack_handle);
        (void) RemoveP6Handle(&g_p6.entry_stack_handle);
        (void) RemoveP6Handle(&g_p6.int64_handle);
        return status == DARTPLANT_OK ? DARTPLANT_HOOK_FAILED : status;
    }
    return DARTPLANT_OK;
}

extern "C" __attribute__((visibility("default"))) uint64_t dartplant_module_external_p6_probe() {
    const uint32_t int64_enter = g_p6.int64_enter.load(std::memory_order_acquire);
    const uint32_t int64_leave = g_p6.int64_leave.load(std::memory_order_acquire);
    const uint32_t entry_enter = g_p6.entry_stack_enter.load(std::memory_order_acquire);
    const uint32_t entry_leave = g_p6.entry_stack_leave.load(std::memory_order_acquire);
    const uint32_t odd_enter = g_p6.odd_stack_enter.load(std::memory_order_acquire);
    const uint32_t odd_leave = g_p6.odd_stack_leave.load(std::memory_order_acquire);
    const uint32_t throw_enter = g_p6.throwing_stack_enter.load(std::memory_order_acquire);
    const uint32_t throw_leave = g_p6.throwing_stack_leave.load(std::memory_order_acquire);
    const uint32_t throw_exception = g_p6.throwing_stack_exception.load(std::memory_order_acquire);
    const uint32_t exception_object =
        g_p6.exception_object_observed.load(std::memory_order_acquire);
    const uint32_t forced_enter = g_p6.forced_stack_enter.load(std::memory_order_acquire);
    const uint32_t forced_leave = g_p6.forced_stack_leave.load(std::memory_order_acquire);
    const uint32_t pair_leave = g_p6.pair_leave.load(std::memory_order_acquire);
    const uint32_t moving_pair = g_p6.moving_gc_pair_leave.load(std::memory_order_acquire);
    const uint32_t failures = g_p6.failures.load(std::memory_order_acquire);
    const bool callbacks = int64_enter == 1 && int64_leave == 1 && entry_enter == 1 &&
                           entry_leave == 1 && odd_enter == 1 && odd_leave == 1 &&
                           throw_enter == 2 && throw_leave == 1 && throw_exception == 1 &&
                           exception_object == 1 && forced_enter == 1 && forced_leave == 1 &&
                           pair_leave == 2 && moving_pair == 1 && failures == 0;
    const bool cleanup =
        RemoveP6Handle(&g_p6.pair_handle) && RemoveP6Handle(&g_p6.forced_stack_handle) &&
        RemoveP6Handle(&g_p6.throwing_stack_handle) && RemoveP6Handle(&g_p6.odd_stack_handle) &&
        RemoveP6Handle(&g_p6.entry_stack_handle) && RemoveP6Handle(&g_p6.int64_handle);
    const bool passed = callbacks && cleanup;
    __android_log_print(passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=p6_probe, state=%s, int64=%u/%u, "
                        "entry_stack=%u/%u, odd_stack=%u/%u, throw=%u/%u/%u, "
                        "exception_object=%u, forced=%u/%u, pair=%u, moving_pair=%u, "
                        "failures=%u, cleanup=%u}",
                        passed ? "pass" : "fail", int64_enter, int64_leave, entry_enter,
                        entry_leave, odd_enter, odd_leave, throw_enter, throw_leave,
                        throw_exception, exception_object, forced_enter, forced_leave, pair_leave,
                        moving_pair, failures, static_cast<unsigned>(cleanup));
    return passed ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) uint64_t
dartplant_module_external_loader_callback_retire() {
    dartplant_lsposed_native_close_callback_admission();
    for (uint32_t attempt = 0; attempt < 100000; ++attempt) {
        if (dartplant_lsposed_native_callback_in_flight() == 0 &&
            dartplant_lsposed_native_finish_callback_drain() != 0) {
            __android_log_print(
                ANDROID_LOG_INFO, "DartPlantModule",
                "DARTPLANT_HOST {event=loader_callback_drain, state=pass, in_flight=0}");
            return 1;
        }
        std::this_thread::yield();
    }
    __android_log_print(ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=loader_callback_drain, state=fail, in_flight=%u}",
                        dartplant_lsposed_native_callback_in_flight());
    return 0;
}

extern "C" __attribute__((visibility("default"))) uint64_t dartplant_module_external_retire() {
    std::lock_guard lock(g_bootstrap_mutex);
    if (g_dart_hook == nullptr || g_vm_adapter == nullptr) return 0;
    if (g_null_hook == nullptr || g_bool_hook == nullptr || g_exception_hook == nullptr ||
        g_root_hook == nullptr) {
        return Fail("semantic_before_retire", DARTPLANT_PROFILE_MISMATCH);
    }
    // Teardown must not require every optional proof to have run in this
    // generation. The first generation exercises the semantic/exception/root/
    // TypeArguments corpus; a rebound generation may exist only to prove
    // owner lifecycle and callback drain. If a probe did run, however, its
    // complete contract remains mandatory before retirement.
    const bool semantic_exercised = g_null_enters.load(std::memory_order_acquire) != 0 ||
                                    g_bool_true.load(std::memory_order_acquire) != 0 ||
                                    g_bool_false.load(std::memory_order_acquire) != 0;
    const bool exception_exercised = g_exception_enter.load(std::memory_order_acquire) != 0 ||
                                     g_exception_unwind.load(std::memory_order_acquire) != 0;
    const bool root_exercised = g_root_leave.load(std::memory_order_acquire) != 0;
    const bool typeargs_exercised = g_typeargs_enter.load(std::memory_order_acquire) != 0;
    if ((semantic_exercised && !SemanticProbe()) || (exception_exercised && !ExceptionProbe()) ||
        (root_exercised && !ObjectRootProbe()) || (typeargs_exercised && !TypeArgsProbe())) {
        return Fail("semantic_before_retire", DARTPLANT_PROFILE_MISMATCH);
    }
    if (g_typeargs_hook != nullptr) {
        const DartPlantStatus typeargs_unhook_status = dartplant_unhook(g_typeargs_hook);
        if (typeargs_unhook_status != DARTPLANT_OK) {
            return Fail("typeargs_unhook", typeargs_unhook_status);
        }
        dartplant_release_hook(g_typeargs_hook);
        g_typeargs_hook = nullptr;
    }
    if (g_typeargs_method != nullptr) {
        dartplant_release_method(g_typeargs_method);
        g_typeargs_method = nullptr;
    }
    const DartPlantStatus root_unhook_status = dartplant_unhook(g_root_hook);
    if (root_unhook_status != DARTPLANT_OK) return Fail("root_unhook", root_unhook_status);
    dartplant_release_hook(g_root_hook);
    g_root_hook = nullptr;
    const DartPlantStatus exception_unhook_status = dartplant_unhook(g_exception_hook);
    if (exception_unhook_status != DARTPLANT_OK)
        return Fail("exception_unhook", exception_unhook_status);
    dartplant_release_hook(g_exception_hook);
    g_exception_hook = nullptr;
    const DartPlantStatus null_unhook_status = dartplant_unhook(g_null_hook);
    if (null_unhook_status != DARTPLANT_OK) return Fail("null_unhook", null_unhook_status);
    dartplant_release_hook(g_null_hook);
    g_null_hook = nullptr;
    const DartPlantStatus bool_unhook_status = dartplant_unhook(g_bool_hook);
    if (bool_unhook_status != DARTPLANT_OK) return Fail("bool_unhook", bool_unhook_status);
    dartplant_release_hook(g_bool_hook);
    g_bool_hook = nullptr;
    const DartPlantStatus unhook_status = dartplant_unhook(g_dart_hook);
    if (unhook_status != DARTPLANT_OK) return Fail("retire_unhook", unhook_status);
    dartplant_release_hook(g_dart_hook);
    g_dart_hook = nullptr;
    const DartPlantStatus retire_status =
        dartplant_flutter_vm_adapter_retire_artifacts(g_vm_adapter);
    if (retire_status != DARTPLANT_OK) return Fail("retire_adapter", retire_status);
    // This fixture keeps libapp/libflutter mapped. Reprove the same physical
    // incarnation before calling the adapter's owner-validated destroy path.
    // Do not report this as a physical dlclose/remap test.
    const DartPlantStatus revalidate_status =
        dartplant_flutter_vm_adapter_revalidate_artifacts(g_vm_adapter);
    if (revalidate_status != DARTPLANT_OK) {
        return Fail("revalidate_before_detach", revalidate_status);
    }
    dartplant_shutdown();
    const DartPlantStatus destroy_status = dartplant_flutter_vm_adapter_destroy(g_vm_adapter);
    if (destroy_status != DARTPLANT_OK) return Fail("destroy_adapter", destroy_status);
    g_vm_adapter = nullptr;
    const auto enter = g_enter.load(std::memory_order_acquire);
    const auto leave = g_leave.load(std::memory_order_acquire);
    const auto rewrite_failures = g_rewrite_failures.load(std::memory_order_acquire);
    const bool passed =
        dartplant_is_initialized() == 0 && enter == leave && enter != 0 && rewrite_failures == 0;
    __android_log_print(passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
                        "DARTPLANT_HOST {event=owner_retire, state=%s, "
                        "initialized=%u, enter=%u, leave=%u, rewrite_failures=%u}",
                        passed ? "pass" : "fail", static_cast<unsigned>(dartplant_is_initialized()),
                        enter, leave, rewrite_failures);
    return passed ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) uint64_t dartplant_module_external_bootstrap(
    uint64_t api_dl_data, uint64_t thr, uint64_t pp, uint64_t heap_bits, uint64_t null_value) {
    std::lock_guard lock(g_bootstrap_mutex);
    if (g_dart_hook != nullptr) {
        return RefreshDeferredOwner(thr, pp, heap_bits, null_value);
    }
    if (api_dl_data == 0 || thr == 0 || pp == 0 || heap_bits == 0 || null_value == 0 ||
        dartplant_flutter_vm_descriptor_count() != 2) {
        return Fail("entry_contract", DARTPLANT_INVALID_ARGUMENT);
    }
    if (dartplant_is_initialized() == 0) {
        const DartPlantInitInfo init = {
            .struct_size = sizeof(DartPlantInitInfo),
            .version = DARTPLANT_INIT_API_VERSION,
            .host_api = g_guest_backend_prepared.load(std::memory_order_acquire)
                            ? dartplant_dobby_host_api()
                            : nullptr,
            .artifact_bundle = nullptr,
            .app_module_name = nullptr,
            .runtime_module_name = nullptr,
        };
        const DartPlantStatus init_status = dartplant_init(&init);
        if (init_status != DARTPLANT_OK) return Fail("owner_thread_init", init_status);
    }
    DartPlantRuntime* runtime = dartplant::DefaultRuntimeInstanceForTesting();
    if (runtime == nullptr) return Fail("default_runtime", DARTPLANT_RUNTIME_NOT_READY);
    const DartPlantStatus image_status =
        dartplant_runtime_on_module_loaded(runtime, "libapp.so", nullptr);
    if (image_status != DARTPLANT_OK) return Fail("image_refresh", image_status);

    // Public SnapshotInfo borrows C strings from the active owner. A module
    // refresh can republish those strings immediately after its API returns.
    // For this test-only, internal runtime consumer, capture *both* immutable
    // strings under the same owner lock instead of racing a borrowed pointer.
    std::string snapshot_hash;
    std::string snapshot_features;
    std::string snapshot_profile;
    {
        std::lock_guard runtime_lock(runtime->mutex);
        const auto& group = dartplant::RuntimeIsolateGroup(runtime);
        if (!group.snapshot.has_value()) return Fail("snapshot", DARTPLANT_RUNTIME_NOT_READY);
        snapshot_hash = group.snapshot->snapshot_hash;
        snapshot_features = group.snapshot->snapshot_features;
        snapshot_profile = group.snapshot->profile_name;
    }
    if (snapshot_hash.empty()) return Fail("snapshot_hash", DARTPLANT_PROFILE_MISMATCH);
    const bool product = snapshot_profile == "flutter-arm64-product-compressed";
    const bool nonproduct = snapshot_profile == "flutter-arm64-profile-compressed";
    if (product == nonproduct) return Fail("snapshot_mode", DARTPLANT_PROFILE_MISMATCH);
    const DartPlantFlutterVmDescriptor* descriptor = nullptr;
    for (uint32_t index = 0; index < dartplant_flutter_vm_descriptor_count(); ++index) {
        const auto* candidate = dartplant_flutter_vm_descriptor_at(index);
        if (candidate == nullptr || candidate->snapshot_hash == nullptr ||
            std::strcmp(candidate->snapshot_hash, snapshot_hash.c_str()) != 0 ||
            candidate->product_mode != static_cast<uint8_t>(product)) {
            continue;
        }
        if (descriptor != nullptr)
            return Fail("ambiguous_fixed_descriptor", DARTPLANT_PROFILE_MISMATCH);
        descriptor = candidate;
    }
    if (descriptor == nullptr) {
        return Fail("fixed_descriptor", DARTPLANT_PROFILE_MISMATCH);
    }
    DartPlantFlutterVmAdapterOptions options{};
    options.struct_size = sizeof(options);
    options.api_version = DARTPLANT_FLUTTER_VM_ADAPTER_API_VERSION;
    options.api_dl_data = reinterpret_cast<void*>(api_dl_data);
    options.thread = thr;
    options.isolate_generation = 1;
    options.snapshot_hash = snapshot_hash.c_str();
    options.snapshot_features = snapshot_features.c_str();
    const DartPlantStatus adapter_status =
        dartplant_flutter_vm_adapter_create(&options, &g_vm_adapter);
    if (adapter_status != DARTPLANT_OK) return Fail("exact_adapter", adapter_status);

    const uint64_t adapter_capabilities = dartplant_flutter_vm_adapter_capabilities(g_vm_adapter);
    const uint64_t expected_create = dartplant::vm_abi::ColdRequiredCapabilityMask();
    const uint64_t expected_verified = dartplant::vm_abi::VerifiedAfterCreateCapabilityMask();
    const uint64_t verified_before =
        dartplant_flutter_vm_adapter_verified_capabilities(g_vm_adapter);
    const uint64_t failed_before = dartplant_flutter_vm_adapter_failed_capabilities(g_vm_adapter);
    const uint64_t generation_before =
        dartplant_flutter_vm_adapter_artifact_generation(g_vm_adapter);
    const DartPlantStatus quiesce = dartplant_flutter_vm_adapter_quiesce_artifacts(g_vm_adapter);
    const uint64_t generation_quiesced =
        dartplant_flutter_vm_adapter_artifact_generation(g_vm_adapter);
    const DartPlantStatus premature_revalidate =
        dartplant_flutter_vm_adapter_revalidate_artifacts(g_vm_adapter);
    const DartPlantStatus retire = dartplant_flutter_vm_adapter_retire_artifacts(g_vm_adapter);
    const uint64_t generation_retired =
        dartplant_flutter_vm_adapter_artifact_generation(g_vm_adapter);
    const uint64_t verified_retired =
        dartplant_flutter_vm_adapter_verified_capabilities(g_vm_adapter);
    const DartPlantStatus revalidate =
        dartplant_flutter_vm_adapter_revalidate_artifacts(g_vm_adapter);
    const uint64_t generation_revalidated =
        dartplant_flutter_vm_adapter_artifact_generation(g_vm_adapter);
    const uint64_t verified_revalidated =
        dartplant_flutter_vm_adapter_verified_capabilities(g_vm_adapter);
    const uint64_t failed_revalidated =
        dartplant_flutter_vm_adapter_failed_capabilities(g_vm_adapter);
    const bool artifact_lifecycle =
        (adapter_capabilities & expected_create) == expected_create &&
        verified_before == expected_verified && failed_before == 0 && generation_before != 0 &&
        quiesce == DARTPLANT_OK && generation_quiesced == generation_before &&
        premature_revalidate == DARTPLANT_VM_ADAPTER_BUSY && retire == DARTPLANT_OK &&
        generation_retired == generation_before + 1 && verified_retired == 0 &&
        revalidate == DARTPLANT_OK && generation_revalidated == generation_retired &&
        verified_revalidated == expected_verified && failed_revalidated == 0;
    __android_log_print(
        artifact_lifecycle ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=artifact_revalidate, state=%s, generation=%llu/%llu/%llu, "
        "quiesce=%d, premature=%d, retire=%d, revalidate=%d}",
        artifact_lifecycle ? "pass" : "fail", static_cast<unsigned long long>(generation_before),
        static_cast<unsigned long long>(generation_retired),
        static_cast<unsigned long long>(generation_revalidated), quiesce, premature_revalidate,
        retire, revalidate);
    if (!artifact_lifecycle) return Fail("artifact_revalidate", DARTPLANT_PROFILE_MISMATCH);

    DartPlantLiveVmArm64Registers registers{};
    registers.struct_size = sizeof(registers);
    registers.thr = thr;
    registers.pp = pp;
    registers.heap_bits = heap_bits;
    registers.null_value = null_value;
    DartPlantLiveVmBootstrapInfo bootstrap{};
    bootstrap.struct_size = sizeof(bootstrap);
    const DartPlantStatus bootstrap_status =
        dartplant_runtime_bootstrap_live_vm_from_arm64_registers_with_adapter(
            runtime, &registers, dartplant_flutter_vm_adapter_get(g_vm_adapter), &bootstrap);
    if (bootstrap_status != DARTPLANT_OK) return Fail("live_vm_bootstrap", bootstrap_status);

    uint32_t initial_image_count = 0;
    const DartPlantStatus initial_count_status =
        dartplant_runtime_get_image_count(runtime, &initial_image_count);
    const DartPlantMethodQuery deferred_query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/deferred_probe.dart",
        .class_name = "Global",
        .function_name = "deferredAdd",
        .signature = nullptr,
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* unexpected_deferred = nullptr;
    const DartPlantStatus initial_deferred_lookup =
        dartplant_runtime_find_method(runtime, &deferred_query, &unexpected_deferred);
    if (unexpected_deferred != nullptr) dartplant_release_method(unexpected_deferred);
    const bool first_generation = !g_deferred_before_ok.load(std::memory_order_acquire);
    const bool deferred_entry_state =
        initial_count_status == DARTPLANT_OK &&
        (first_generation
             ? initial_image_count == 1 && initial_deferred_lookup == DARTPLANT_METHOD_NOT_FOUND
             : initial_image_count >= 1 && (initial_deferred_lookup == DARTPLANT_METHOD_NOT_FOUND ||
                                            initial_deferred_lookup == DARTPLANT_OK));
    if (first_generation && deferred_entry_state) {
        g_deferred_before_ok.store(true, std::memory_order_release);
    }
    __android_log_print(
        deferred_entry_state ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=deferred_entry_state, state=%s, first_generation=%u, "
        "image_count=%u, lookup=%d}",
        deferred_entry_state ? "pass" : "fail", static_cast<unsigned>(first_generation),
        initial_image_count, initial_deferred_lookup);
    if (!deferred_entry_state) {
        return Fail("deferred_entry_state", DARTPLANT_RUNTIME_NOT_READY);
    }
    if (first_generation) {
        __android_log_print(
            ANDROID_LOG_INFO, "DartPlantModule",
            "DARTPLANT_HOST {event=deferred_before_load, state=pass, image_count=%u, lookup=%d}",
            initial_image_count, initial_deferred_lookup);
    }

    // Prove the exact external AOT artifact before publishing ANY physical
    // Hook. Otherwise a mismatched sidecar could reject the final root hook
    // after ordinary/null/bool/exception code has already been patched.
    const DartPlantMethodQuery root_query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "Global",
        .function_name = "externalObjectRootProbe",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* root_method = nullptr;
    const DartPlantStatus root_lookup =
        dartplant_runtime_find_method(runtime, &root_query, &root_method);
    if (root_lookup != DARTPLANT_OK) return Fail("external_root_lookup", root_lookup);
    const DartPlantStatus root_evidence = dartplant_runtime_register_compiler_abi_evidence(
        runtime, root_method, &kDartPlantExternalRootAbiEvidence);
    if (root_evidence != DARTPLANT_OK) {
        dartplant_release_method(root_method);
        const bool before_publish = g_dart_hook == nullptr && g_null_hook == nullptr &&
                                    g_bool_hook == nullptr && g_exception_hook == nullptr &&
                                    g_root_hook == nullptr;
        __android_log_print(before_publish ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                            "DartPlantModule",
                            "DARTPLANT_HOST {event=artifact_preflight, state=reject, "
                            "before_hook_publish=%u, status=%d}",
                            static_cast<unsigned>(before_publish), root_evidence);
        return Fail("external_root_preflight", root_evidence);
    }
    DartPlantMethodAbiInfo root_abi{};
    root_abi.struct_size = sizeof(root_abi);
    const DartPlantStatus root_info =
        dartplant_runtime_get_method_abi_info(runtime, root_method, &root_abi);
    dartplant_release_method(root_method);
    if (root_info != DARTPLANT_OK || root_abi.state != DARTPLANT_METHOD_ABI_VERIFIED ||
        root_abi.has_verified_call_layout == 0) {
        return Fail("external_root_abi",
                    root_info != DARTPLANT_OK ? root_info : DARTPLANT_UNSUPPORTED_ABI);
    }

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(DartPlantMethodQuery),
        .library_uri = "package:dartplant_fixture/main.dart",
        .class_name = "Global",
        .function_name = "instrumentedAdd",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    const DartPlantStatus find_status = dartplant_runtime_find_method(runtime, &query, &method);
    if (find_status != DARTPLANT_OK) return Fail("live_function_lookup", find_status);

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT |
                    DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS | DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    profile.argument_count = 2;
    profile.argument_locations[0] = {DARTPLANT_ABI_GP_REGISTER, 1, {0, 0}};
    profile.argument_locations[1] = {DARTPLANT_ABI_GP_REGISTER, 2, {0, 0}};
    profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
    DartPlantHookOptions hook_options{};
    hook_options.struct_size = sizeof(hook_options);
    hook_options.flags = DARTPLANT_HOOK_ALLOW_SHARED_CODE;
    hook_options.on_enter = OnEnter;
    hook_options.on_leave = OnLeave;
    const DartPlantStatus hook_status = dartplant_runtime_hook_method_with_profile(
        runtime, method, &profile, &hook_options, &g_dart_hook);
    dartplant_release_method(method);
    if (hook_status != DARTPLANT_OK) return Fail("external_host_hook", hook_status);
    const auto install_semantic = [&](const char* function_name, bool shared,
                                      DartPlantInvocationCallback on_enter,
                                      DartPlantInvocationCallback on_leave, DartPlantHook** output,
                                      bool object_bridge = false) {
        const DartPlantMethodQuery semantic_query = {
            .struct_size = sizeof(DartPlantMethodQuery),
            .library_uri = "package:dartplant_fixture/main.dart",
            .class_name = "Global",
            .function_name = function_name,
            .signature = "",
            .entry_kind = DARTPLANT_ENTRY_DEFAULT,
        };
        DartPlantMethod* semantic_method = nullptr;
        const DartPlantStatus lookup =
            dartplant_runtime_find_method(runtime, &semantic_query, &semantic_method);
        if (lookup != DARTPLANT_OK) return lookup;
        if (object_bridge) {
            DartPlantMethodAbiInfo abi_info{};
            abi_info.struct_size = sizeof(abi_info);
            const DartPlantStatus info_status =
                dartplant_runtime_get_method_abi_info(runtime, semantic_method, &abi_info);
            if (info_status != DARTPLANT_OK || abi_info.state != DARTPLANT_METHOD_ABI_VERIFIED ||
                abi_info.has_verified_call_layout == 0) {
                dartplant_release_method(semantic_method);
                return info_status != DARTPLANT_OK ? info_status : DARTPLANT_UNSUPPORTED_ABI;
            }
        }
        DartPlantRuntimeProfile semantic_profile{};
        dartplant_runtime_profile_init_arm64_aot(&semantic_profile);
        // As in the internal fixture, optimized arguments have no assumed
        // x1/x2 slot here. Both semantic hooks prove only their tagged result.
        semantic_profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS |
                                 DARTPLANT_PROFILE_RAW_GP_RESULT |
                                 DARTPLANT_PROFILE_TAGGED_GP_RESULT;
        semantic_profile.argument_count = 0;
        semantic_profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
        DartPlantHookOptions semantic_options{};
        semantic_options.struct_size = sizeof(semantic_options);
        semantic_options.flags = shared ? DARTPLANT_HOOK_ALLOW_SHARED_CODE : 0;
        semantic_options.on_enter = on_enter;
        semantic_options.on_leave = on_leave;
        semantic_options.vm_adapter =
            object_bridge ? dartplant_flutter_vm_adapter_get(g_vm_adapter) : nullptr;
        const DartPlantStatus installed = dartplant_runtime_hook_method_with_profile(
            runtime, semantic_method, &semantic_profile, &semantic_options, output);
        dartplant_release_method(semantic_method);
        return installed;
    };
    const DartPlantStatus null_status =
        install_semantic("nullableEchoObject", false, OnNullEnter, OnNullLeave, &g_null_hook);
    if (null_status != DARTPLANT_OK) return Fail("external_null_hook", null_status);
    const DartPlantStatus bool_status =
        install_semantic("negateBool", true, nullptr, OnBoolLeave, &g_bool_hook);
    if (bool_status != DARTPLANT_OK) return Fail("external_bool_hook", bool_status);
    const DartPlantStatus exception_status = install_semantic(
        "externalThrowingProbe", true, OnExceptionEnter, OnExceptionLeave, &g_exception_hook);
    if (exception_status != DARTPLANT_OK) return Fail("external_exception_hook", exception_status);
    if (g_exception_hook->listeners.size() != 1 || g_exception_hook->listeners.front() == nullptr) {
        return Fail("external_exception_listener", DARTPLANT_RUNTIME_NOT_READY);
    }
    g_exception_hook->listeners.front()->on_exception.store(OnExceptionUnwind,
                                                            std::memory_order_release);
    const DartPlantStatus root_status = install_semantic("externalObjectRootProbe", false, nullptr,
                                                         OnObjectRootLeave, &g_root_hook, true);
    if (root_status != DARTPLANT_OK) return Fail("external_root_hook", root_status);
    __android_log_print(ANDROID_LOG_INFO, "DartPlantModule",
                        "DARTPLANT_HOST {event=owner_entry, state=hooked, profile=%s, "
                        "candidates=%u}",
                        descriptor->descriptor_id, bootstrap.validated_candidates);
    return 1;
}
