// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <android/log.h>

#include <atomic>

#include "core/internal.h"
#include "runtime/runtime_internal.h"

namespace {

constexpr char kLogTag[] = "DartPlant";
std::atomic_bool g_initialized{false};

void OnModuleLoaded(const char* name, void* handle) {
    dartplant::RefreshModules();
    const DartPlantStatus status = dartplant::NotifyRuntimeModuleLoaded(name, handle);
    if (status != DARTPLANT_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "runtime module refresh failed: %s",
                            dartplant::LastError());
    }
    if (name != nullptr) {
        __android_log_print(ANDROID_LOG_DEBUG, kLogTag, "module loaded: %s", name);
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) DartPlantNativeOnModuleLoaded
native_init(const DartPlantNativeApiEntries* entries) {
    if (entries == nullptr || entries->version < 2 || entries->hook_func == nullptr ||
        entries->unhook_func == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "invalid LSPosed Native API entries");
        return nullptr;
    }
    if (g_initialized.exchange(true, std::memory_order_acq_rel)) {
        __android_log_print(ANDROID_LOG_DEBUG, kLogTag,
                            "ignored duplicate LSPosed native_init invocation");
        return nullptr;
    }
    dartplant::InstallHostApi(entries);
    dartplant::RefreshModules();
    // LSPosed reports only future successful dlopen calls, so cover images that
    // were already mapped before this native module was initialized.
    const DartPlantStatus status = dartplant::NotifyRuntimeModuleLoaded(nullptr, nullptr);
    if (status != DARTPLANT_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "initial runtime refresh failed: %s",
                            dartplant::LastError());
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "initialized LSPosed Native API version %u",
                        entries->version);
    return OnModuleLoaded;
}
