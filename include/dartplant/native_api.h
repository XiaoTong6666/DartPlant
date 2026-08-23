// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_NATIVE_API_H_
#define DARTPLANT_NATIVE_API_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*DartPlantHostHook)(void* target, void* replacement, void** backup);
typedef int (*DartPlantHostUnhook)(void* target);
typedef void (*DartPlantNativeOnModuleLoaded)(const char* name, void* handle);

typedef struct DartPlantNativeApiEntries {
    uint32_t version;
    DartPlantHostHook hook_func;
    DartPlantHostUnhook unhook_func;
} DartPlantNativeApiEntries;

typedef DartPlantNativeOnModuleLoaded (*DartPlantNativeInit)(
    const DartPlantNativeApiEntries* entries);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_NATIVE_API_H_
