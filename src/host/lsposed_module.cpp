// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <android/log.h>

#include <atomic>

#include "core/internal.h"
#include "runtime/runtime_internal.h"

namespace {

constexpr char kLogTag[] = "DartPlant";
std::atomic_bool g_initialized{false};

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
    dartplant::InstallHostApi(entries);
    dartplant::StartRuntimeModuleRefreshWorker(ReportRuntimeRefresh);
    // Vector reports only future successful dlopen calls. Queue the existing
    // mapping scan instead of running it under Vector's registry mutex.
    dartplant::ScheduleRuntimeModuleRefresh();
    return OnModuleLoaded;
}
