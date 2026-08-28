// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_NATIVE_API_H_
#define DARTPLANT_NATIVE_API_H_

#include "dartplant/host_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DartPlantNativeOnModuleLoaded)(const char* name, void* handle);
typedef int (*DartPlantNativeHook)(void* target, void* replacement, void** backup);
typedef int (*DartPlantNativeUnhook)(void* target);

// Source-compatibility aliases kept for consumers of the original public C
// header. The native_init ABI is unchanged; new generic hosts should use
// DartPlantHostApi from host_api.h.
typedef DartPlantNativeHook DartPlantHostHook;
typedef DartPlantNativeUnhook DartPlantHostUnhook;

// Compatibility ABI used by LSPosed/Vector native_init hosts. DartPlant core
// itself consumes DartPlantHostApi; this adapter type intentionally preserves
// the established field names and version=2 contract.
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

#endif  // DARTPLANT_NATIVE_API_H_
