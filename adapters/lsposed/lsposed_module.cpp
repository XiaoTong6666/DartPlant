// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <android/log.h>

#include <atomic>

#include "dartplant/adapters/lsposed_native_api.h"
#include "dartplant/host_api.h"
#include "runtime/runtime_internal.h"

namespace {

constexpr char kLogTag[] = "DartPlant";
std::atomic_bool g_initialized{false};

struct LsposedHostState {
    DartPlantNativeHook hook = nullptr;
    DartPlantNativeUnhook unhook = nullptr;
};

LsposedHostState g_host;

int HostHook(void* user_data, void* target, void* replacement, void** backup) {
    const auto* host = static_cast<const LsposedHostState*>(user_data);
    return host == nullptr || host->hook == nullptr ? -1 : host->hook(target, replacement, backup);
}

int HostUnhook(void* user_data, void* target) {
    const auto* host = static_cast<const LsposedHostState*>(user_data);
    return host == nullptr || host->unhook == nullptr ? -1 : host->unhook(target);
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
    // Vector invokes this while holding g_module_registry_mutex. Keep this path
    // to atomic filtering/coalescing only; the worker performs every heavy step.
    (void) name;
    dartplant::ScheduleRuntimeModuleRefresh();
}

}  // namespace

extern "C" __attribute__((visibility("default"))) DartPlantNativeOnModuleLoaded
native_init(const DartPlantNativeApiEntries* entries) {
    if (entries == nullptr || entries->version < 2 || entries->hook_func == nullptr ||
        entries->unhook_func == nullptr) {
        return nullptr;
    }
    if (g_initialized.exchange(true, std::memory_order_acq_rel)) {
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
    };
    if (dartplant_install_host_api(&host_api) != DARTPLANT_OK) {
        g_initialized.store(false, std::memory_order_release);
        return nullptr;
    }
    const DartPlantInitInfo init = {
        .struct_size = sizeof(DartPlantInitInfo),
        .version = DARTPLANT_INIT_API_VERSION,
        .host_api = nullptr,
        .artifact_bundle = nullptr,
        .app_module_name = nullptr,
        .runtime_module_name = nullptr,
    };
    const DartPlantStatus init_status = dartplant_init(&init);
    if (init_status != DARTPLANT_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "default DartPlant runtime init failed (%d): %s",
                            static_cast<int>(init_status), dartplant_last_error());
        g_initialized.store(false, std::memory_order_release);
        return nullptr;
    }
    dartplant::StartRuntimeModuleRefreshWorker(ReportRuntimeRefresh);
    // Vector reports only future successful dlopen calls. Queue the existing
    // mapping scan instead of running it under Vector's registry mutex.
    dartplant::ScheduleRuntimeModuleRefresh();
    return OnModuleLoaded;
}
