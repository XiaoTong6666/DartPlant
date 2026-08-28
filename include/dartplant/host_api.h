// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_HOST_API_H_
#define DARTPLANT_HOST_API_H_

#include <stdint.h>

#include "dartplant/dartplant.h"

#ifdef __cplusplus
extern "C" {
#endif

// Minimal host contract required by DartPlant core. The host owns the actual
// inline-hook implementation (LSPosed Native API, Dobby, ShadowHook, etc.).
// user_data lets a consumer bind a backend instance without process-global
// state; DartPlant retains this binding for the lifetime of every installed
// physical hook so backend replacement cannot redirect a later unhook.
typedef int (*DartPlantHostHookCallback)(void* user_data, void* target, void* replacement,
                                         void** backup);
typedef int (*DartPlantHostUnhookCallback)(void* user_data, void* target);

typedef struct DartPlantHostApi {
    uint32_t struct_size;
    uint32_t version;
    void* user_data;
    DartPlantHostHookCallback hook;
    DartPlantHostUnhookCallback unhook;
} DartPlantHostApi;

enum { DARTPLANT_HOST_API_VERSION = 1 };

DARTPLANT_EXPORT DartPlantStatus dartplant_install_host_api(const DartPlantHostApi* api);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_HOST_API_H_
