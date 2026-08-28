// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_HOOK_H_
#define DARTPLANT_HOOK_H_

#include <stdint.h>

#include "dartplant/dartplant.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DartPlantInvocation DartPlantInvocation;
typedef struct DartPlantVmAdapter DartPlantVmAdapter;
typedef struct DartPlantHookHandle DartPlantHookHandle;

typedef void (*DartPlantInvocationCallback)(DartPlantInvocation* invocation, void* user_data);

typedef enum DartPlantHookFlags {
    // A shared AOT Code entry can represent multiple logical Dart Functions.
    // Normal method callbacks fail closed unless the consumer explicitly asks
    // for a physical shared-Code subscription. Typed ABI interpretation is
    // still suppressed while the logical identity is ambiguous.
    DARTPLANT_HOOK_ALLOW_SHARED_CODE = 1u << 0,
} DartPlantHookFlags;

typedef struct DartPlantHookOptions {
    uint32_t struct_size;
    uint32_t flags;
    DartPlantInvocationCallback on_enter;
    DartPlantInvocationCallback on_leave;
    void* user_data;
    // Advanced/optional VM object bridge. Normal consumers leave this null.
    DartPlantVmAdapter* vm_adapter;
} DartPlantHookOptions;

// Installs one logical method subscription. DartPlant owns the distinction
// between an existing/new physical CodeTarget hook and an additional listener;
// every successful call returns the same public logical-handle abstraction.
DARTPLANT_EXPORT DartPlantStatus dartplant_hook_method(const DartPlantMethod* method,
                                                       const DartPlantHookOptions* options,
                                                       DartPlantHookHandle** out_handle);

// Removes this logical subscription. The physical trampoline is removed only
// when the final subscription for its CodeTarget disappears.
DARTPLANT_EXPORT DartPlantStatus dartplant_unhook_handle(DartPlantHookHandle* handle);
DARTPLANT_EXPORT uint8_t dartplant_hook_handle_is_active(const DartPlantHookHandle* handle);
DARTPLANT_EXPORT uint8_t dartplant_hook_handle_is_idle(const DartPlantHookHandle* handle);
// Release after unhook_handle() and is_idle()==1. If an invocation still owns
// the listener snapshot, the handle is retained and last_error is set.
DARTPLANT_EXPORT void dartplant_release_hook_handle(DartPlantHookHandle* handle);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_HOOK_H_
