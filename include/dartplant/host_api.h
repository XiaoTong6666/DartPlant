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
// state; DartPlant retains the pointer value together with the callback binding
// for every installed physical hook so backend replacement cannot redirect a
// later unhook. user_data is borrowed, not owned: the host must keep the
// pointed-to backend instance alive until every hook created with this binding
// has been unhooked and released. dartplant_shutdown() clears the current
// default binding so a later dartplant_init(... host_api=nullptr) cannot reuse a
// stale backend accidentally; already-created physical hooks retain their own
// immutable callback binding for safe teardown.
typedef int (*DartPlantHostHookCallback)(void* user_data, void* target, void* replacement,
                                         void** backup);
typedef int (*DartPlantHostUnhookCallback)(void* user_data, void* target);

struct DartPlantHostApi {
    uint32_t struct_size;
    uint32_t version;
    void* user_data;
    DartPlantHostHookCallback hook;
    DartPlantHostUnhookCallback unhook;
};

enum { DARTPLANT_HOST_API_VERSION = 1 };

DARTPLANT_EXPORT DartPlantStatus dartplant_install_host_api(const DartPlantHostApi* api);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_HOST_API_H_
