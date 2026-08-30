// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_HOST_API_H_
#define DARTPLANT_HOST_API_H_

#include <stddef.h>
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
// The low-level callback keeps the original ABI: zero means success and an
// unknown non-zero value is treated as failed-before-publication for backwards
// compatibility. New adapters should return one of these reserved values so
// DartPlant can preserve executable ownership when a failed transaction had
// already made replacement reachable.
enum DartPlantHostHookResult {
    DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED = -1,
    DARTPLANT_HOST_HOOK_FAILED_AFTER_PUBLISHED = -2,
};

typedef void (*DartPlantHostBackupReadyCallback)(void* user_data, void* backup);

typedef struct DartPlantHostHookTransaction {
    uint32_t struct_size;
    void* user_data;
    DartPlantHostBackupReadyCallback backup_ready;
} DartPlantHostHookTransaction;

typedef int (*DartPlantHostHookWithPublicationCallback)(void* user_data, void* target,
                                                        void* replacement,
                                                        DartPlantHostHookTransaction* transaction);

// FAILED_NEVER_PUBLISHED is failure-atomic: target is unchanged when this call
// returns and the adapter has rolled back any entry it created itself. Core
// will not call unhook(target) after a failed hook, because target may already
// belong to another backend user. FAILED_AFTER_PUBLISHED means the adapter has
// restored target before returning, but replacement may have been fetched by a
// CPU; core therefore retains its callback entry/HookRecord for process life.
// On success the backend must write a callable original trampoline to *backup
// before replacement is made reachable and must not return a partially
// published transaction.
typedef int (*DartPlantHostHookCallback)(void* user_data, void* target, void* replacement,
                                         void** backup);
typedef int (*DartPlantHostUnhookCallback)(void* user_data, void* target);

struct DartPlantHostApi {
    uint32_t struct_size;
    uint32_t version;
    void* user_data;
    DartPlantHostHookCallback hook;
    DartPlantHostUnhookCallback unhook;
    // Optional strict path. Real Dart callback hooks require this path. The
    // adapter must call transaction->backup_ready() after producing the
    // original trampoline and before publishing target.
    DartPlantHostHookWithPublicationCallback hook_with_publication;
};

#define DARTPLANT_HOST_API_LEGACY_SIZE offsetof(DartPlantHostApi, hook_with_publication)

enum { DARTPLANT_HOST_API_VERSION = 1 };

DARTPLANT_EXPORT DartPlantStatus dartplant_install_host_api(const DartPlantHostApi* api);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_HOST_API_H_
