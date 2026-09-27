// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>

#include "core/internal.h"
#include "dartplant/adapters/lsposed_native_api.h"
#include "dartplant/host_api.h"
#include "runtime/runtime_internal.h"

namespace {

constexpr char kLogTag[] = "DartPlant";
std::atomic_bool g_initialized{false};

enum class LoaderCallbackState : uint32_t {
    kActive = 0,
    kDraining = 1,
    kClosed = 2,
};

constexpr uint64_t kLoaderCallbackEntrant = uint64_t{1} << 32;
constexpr uint64_t kLoaderCallbackStateMask = 0xffffffffULL;
std::atomic_uint64_t g_loader_callback_control{static_cast<uint64_t>(LoaderCallbackState::kActive)};

bool TryEnterLoaderCallback() {
    auto current = g_loader_callback_control.load(std::memory_order_acquire);
    for (;;) {
        if (static_cast<uint32_t>(current) != static_cast<uint32_t>(LoaderCallbackState::kActive) ||
            (current >> 32) == std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        if (g_loader_callback_control.compare_exchange_weak(
                current, current + kLoaderCallbackEntrant, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

void ExitLoaderCallback() {
    g_loader_callback_control.fetch_sub(kLoaderCallbackEntrant, std::memory_order_acq_rel);
}

void CloseLoaderCallbackAdmission() {
    auto current = g_loader_callback_control.load(std::memory_order_acquire);
    while (static_cast<uint32_t>(current) == static_cast<uint32_t>(LoaderCallbackState::kActive)) {
        const auto next = (current & ~kLoaderCallbackStateMask) |
                          static_cast<uint32_t>(LoaderCallbackState::kDraining);
        if (g_loader_callback_control.compare_exchange_weak(
                current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
}

bool FinishLoaderCallbackDrain() {
    auto expected = static_cast<uint64_t>(LoaderCallbackState::kDraining);
    return g_loader_callback_control.compare_exchange_strong(
               expected, static_cast<uint64_t>(LoaderCallbackState::kClosed),
               std::memory_order_acq_rel, std::memory_order_acquire) ||
           expected == static_cast<uint64_t>(LoaderCallbackState::kClosed);
}

uint32_t LoaderCallbackInFlight() {
    return static_cast<uint32_t>(g_loader_callback_control.load(std::memory_order_acquire) >> 32);
}

struct LsposedHostState {
    DartPlantNativeHook hook = nullptr;
    DartPlantNativeUnhook unhook = nullptr;
};

LsposedHostState g_host;

#if defined(DARTPLANT_FIXED_VM_FAMILY)
// Test-only preflight: the framework must be able to unhook its *own* two
// Dobby entries in either order. The first entry is intentionally removed
// with the second still installed, matching the AOT entry + RET scenario.
// This checks the framework's registry independently of DartPlant HookRecord
// ownership. Never use it as an alternative unhook implementation.
__attribute__((noinline, used)) uint64_t NativeSelfTestTargetA(uint64_t arg) {
    __asm__ volatile(".rept 8\n nop\n .endr" ::: "memory");
    return arg + 1;
}
__attribute__((noinline, used)) uint64_t NativeSelfTestTargetB(uint64_t arg) {
    __asm__ volatile(".rept 8\n nop\n .endr" ::: "memory");
    return arg + 1;
}
__attribute__((noinline, used)) uint64_t NativeSelfTestReplacementA(uint64_t arg) {
    return arg + 10;
}
__attribute__((noinline, used)) uint64_t NativeSelfTestReplacementB(uint64_t arg) {
    return arg + 20;
}
std::mutex g_selftest_mutex;
bool g_selftest_ran = false;
bool g_selftest_passed = false;
#endif

int HostHook(void* user_data, void* target, void* replacement, void** backup) {
    const auto* host = static_cast<const LsposedHostState*>(user_data);
    const int status =
        host == nullptr || host->hook == nullptr ? -1 : host->hook(target, replacement, backup);
    __android_log_print(status == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kLogTag,
                        "DARTPLANT_HOST {event=native_v2_hook, status=%d, target=%p, "
                        "replacement=%p, backup=%p, hook_callback=%p}",
                        status, target, replacement, backup == nullptr ? nullptr : *backup,
                        host == nullptr ? nullptr : reinterpret_cast<void*>(host->hook));
    return status;
}

int HostUnhook(void* user_data, void* target) {
    const auto* host = static_cast<const LsposedHostState*>(user_data);
    const int status = host == nullptr || host->unhook == nullptr ? -1 : host->unhook(target);
    __android_log_print(status == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kLogTag,
                        "DARTPLANT_HOST {event=native_v2_unhook, status=%d, target=%p, "
                        "unhook_callback=%p}",
                        status, target,
                        host == nullptr ? nullptr : reinterpret_cast<void*>(host->unhook));
    return status;
}

__attribute__((constructor)) void InitializeRuntimeRefreshWorker() {
    // Constructors run during do_dlopen before Vector acquires its module
    // registry mutex and invokes native_init.
    dartplant::StartRuntimeModuleRefreshWorker(nullptr);
}

void ReportRuntimeRefresh(DartPlantStatus status, const char* error) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "runtime module refresh failed (%d): %s",
                        static_cast<int>(status), error == nullptr ? "unknown error" : error);
}

void OnModuleLoaded(const char* name, void*) {
    if (!TryEnterLoaderCallback()) return;
    struct Exit {
        ~Exit() { ExitLoaderCallback(); }
    } exit;
    // The host may invoke this during nested loader activity. Keep this path
    // to atomic filtering/coalescing only; the worker performs every heavy step.
    (void) name;
    dartplant::ScheduleRuntimeModuleRefresh();
}

}  // namespace

// The Native API loader can initialize us before Flutter's libapp/libflutter
// mappings exist. Expose readiness of the stable HostApi callback binding,
// not readiness of a VM owner that has not yet been created.
extern "C" __attribute__((visibility("default"))) uint8_t dartplant_lsposed_native_host_ready() {
    return g_initialized.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) void
dartplant_lsposed_native_close_callback_admission() {
    CloseLoaderCallbackAdmission();
}

extern "C" __attribute__((visibility("default"))) uint8_t
dartplant_lsposed_native_finish_callback_drain() {
    return FinishLoaderCallbackDrain() ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) uint32_t
dartplant_lsposed_native_callback_in_flight() {
    return LoaderCallbackInFlight();
}

#if defined(DARTPLANT_FIXED_VM_FAMILY)
extern "C"
    __attribute__((visibility("default"))) uint8_t dartplant_lsposed_native_backend_selftest() {
    std::lock_guard lock(g_selftest_mutex);
    if (g_selftest_ran) return g_selftest_passed ? 1 : 0;
    g_selftest_ran = true;
    if (!g_initialized.load(std::memory_order_acquire) || g_host.hook == nullptr ||
        g_host.unhook == nullptr) {
        return 0;
    }
    using Probe = uint64_t (*)(uint64_t);
    void* backup_a = nullptr;
    void* backup_b = nullptr;
    const auto target_a = reinterpret_cast<void*>(&NativeSelfTestTargetA);
    const auto target_b = reinterpret_cast<void*>(&NativeSelfTestTargetB);
    const auto replace_a = reinterpret_cast<void*>(&NativeSelfTestReplacementA);
    const auto replace_b = reinterpret_cast<void*>(&NativeSelfTestReplacementB);
    const int hook_a = g_host.hook(target_a, replace_a, &backup_a);
    const int hook_b = hook_a == 0 ? g_host.hook(target_b, replace_b, &backup_b) : -1;
    const bool hook_proved =
        hook_a == 0 && hook_b == 0 && backup_a != nullptr && backup_b != nullptr &&
        reinterpret_cast<Probe>(target_a)(5) == 15 && reinterpret_cast<Probe>(target_b)(5) == 25 &&
        reinterpret_cast<Probe>(backup_a)(5) == 6 && reinterpret_cast<Probe>(backup_b)(5) == 6;
    const int unhook_a = hook_a == 0 ? g_host.unhook(target_a) : -1;
    const int unhook_b = hook_b == 0 ? g_host.unhook(target_b) : -1;
    const bool restored = unhook_a == 0 && unhook_b == 0 &&
                          reinterpret_cast<Probe>(target_a)(5) == 6 &&
                          reinterpret_cast<Probe>(target_b)(5) == 6;
    g_selftest_passed = hook_proved && restored;
    __android_log_print(g_selftest_passed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kLogTag,
                        "DARTPLANT_HOST {event=native_v2_selftest, state=%s, "
                        "hook_a=%d, hook_b=%d, unhook_a=%d, unhook_b=%d, "
                        "backup_a=%u, backup_b=%u, hook_proved=%u, restored=%u}",
                        g_selftest_passed ? "pass" : "fail", hook_a, hook_b, unhook_a, unhook_b,
                        static_cast<unsigned>(backup_a != nullptr),
                        static_cast<unsigned>(backup_b != nullptr),
                        static_cast<unsigned>(hook_proved), static_cast<unsigned>(restored));
    return g_selftest_passed ? 1 : 0;
}
#endif
extern "C" __attribute__((visibility("default"))) DartPlantNativeOnModuleLoaded
native_init(const DartPlantNativeApiEntries* entries) {
    if (entries == nullptr || entries->version < 2 || entries->hook_func == nullptr ||
        entries->unhook_func == nullptr) {
        return nullptr;
    }
    if (!TryEnterLoaderCallback()) return nullptr;
    struct InitExit {
        ~InitExit() { ExitLoaderCallback(); }
    } init_exit;
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return nullptr;
    }
    g_host = {
        .hook = entries->hook_func,
        .unhook = entries->unhook_func,
    };
    const DartPlantHostApi host_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &g_host,
        .hook = HostHook,
        .unhook = HostUnhook,
        .hook_with_publication = nullptr,
    };
    // The LSPosed/Vector Native API v2 implementations audited by DartPlant are
    // synchronous Dobby wrappers: a successful backup remains callable until
    // unhook, and every reported hook failure occurs before Dobby Commit().
    // They can still publish replacement before hook() returns its backup, so
    // DartPlant never exposes the real callback directly to that ABI. The
    // local publication gate remains closed until the call returns.
    dartplant::InstallHostApi(&host_api, dartplant::HostPublicationPolicy::kLocalGate);
    // Do not create the default runtime here: Vector/LSPosed calls native_init
    // while Flutter has not yet dlopened libflutter and libapp. The first
    // exact owner-thread FFI invocation creates the runtime after the images
    // have actually been mapped. Otherwise a process tree with zero engine
    // owners can never be refreshed into a valid Flutter owner.
    dartplant::StartRuntimeModuleRefreshWorker(ReportRuntimeRefresh);
    // Vector reports only future successful dlopen calls. Queue the existing
    // mapping scan instead of running it under Vector's registry mutex.
    dartplant::ScheduleRuntimeModuleRefresh();
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "DARTPLANT_HOST {event=native_v2_init, state=host_ready}");
    return OnModuleLoaded;
}
