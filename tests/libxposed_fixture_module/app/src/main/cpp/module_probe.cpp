// Test-only JNI/FFI entrypoints live inside the SAME libdartplant.so as the
// Native API host and exact VM adapter. No second copy of dartplant_core.
#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "dartplant/adapters/dobby.h"
#include "dartplant/adapters/flutter_vm.h"
#include "dartplant/adapters/lsposed_native_api.h"
#include "dartplant/dartplant.h"
#include "dartplant/hook.h"
#include "dartplant/runtime.h"
#include "external_root_sidecar.h"
#include "runtime/default_runtime.h"
#include "runtime/runtime_internal.h"

#if !defined(DARTPLANT_DART_PLANT_EXTERNAL_ROOT_ABI_EVIDENCE_AVAILABLE) ||                         \
    DARTPLANT_DART_PLANT_EXTERNAL_ROOT_ABI_EVIDENCE_AVAILABLE != 1
#error "External VM-root proof needs an exact compiler/AOT sidecar for this fixture"
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
std::atomic_bool g_guest_backend_prepared{false};

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
    __android_log_print(
        ANDROID_LOG_ERROR, "DartPlantModule",
        "DARTPLANT_HOST {event=loader_callback_drain, state=fail, in_flight=%u}",
        dartplant_lsposed_native_callback_in_flight());
    return 0;
}

extern "C" __attribute__((visibility("default"))) uint64_t dartplant_module_external_retire() {
    std::lock_guard lock(g_bootstrap_mutex);
    if (g_dart_hook == nullptr || g_vm_adapter == nullptr) return 0;
    if (g_null_hook == nullptr || g_bool_hook == nullptr || g_exception_hook == nullptr ||
        g_root_hook == nullptr || !SemanticProbe() || !ExceptionProbe() || !ObjectRootProbe()) {
        return Fail("semantic_before_retire", DARTPLANT_PROFILE_MISMATCH);
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
    if (g_dart_hook != nullptr) return 1;
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
