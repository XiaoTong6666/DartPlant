// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ADAPTERS_LSPOSED_NATIVE_API_H_
#define DARTPLANT_ADAPTERS_LSPOSED_NATIVE_API_H_

#include "dartplant/host_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DartPlantNativeOnModuleLoaded)(const char* name, void* handle);
typedef int (*DartPlantNativeHook)(void* target, void* replacement, void** backup);
typedef int (*DartPlantNativeUnhook)(void* target);

// Source-compatibility aliases from the original native_api.h surface.
typedef DartPlantNativeHook DartPlantHostHook;
typedef DartPlantNativeUnhook DartPlantHostUnhook;

// LSPosed/Vector native_init compatibility ABI. This type belongs to the
// adapter boundary; dartplant_core consumes only DartPlantHostApi.
typedef struct DartPlantNativeApiEntries {
    uint32_t version;
    DartPlantNativeHook hook_func;
    DartPlantNativeUnhook unhook_func;
} DartPlantNativeApiEntries;

typedef DartPlantNativeOnModuleLoaded (*DartPlantNativeInit)(
    const DartPlantNativeApiEntries* entries);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_ADAPTERS_LSPOSED_NATIVE_API_H_
