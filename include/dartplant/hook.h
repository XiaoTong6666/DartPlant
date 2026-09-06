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
typedef void (*DartPlantExceptionCallback)(DartPlantInvocation* invocation, void* user_data);

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
    // Advanced/optional VM object bridge. Adapter V3 supplies the GC-safe
    // Generated<->Native transition and VM-visible roots required by real
    // Dart-entry callbacks. Normal scalar-only consumers leave this null.
    DartPlantVmAdapter* vm_adapter;
} DartPlantHookOptions;

// Installs one logical method subscription. DartPlant owns the distinction
// between an existing/new physical entry-target hook and an additional listener;
// every successful call returns the same public logical-handle abstraction.
DARTPLANT_EXPORT DartPlantStatus dartplant_hook_method(const DartPlantMethod* method,
                                                       const DartPlantHookOptions* options,
                                                       DartPlantHookHandle** out_handle);

// Removes this logical subscription. The physical trampoline is removed only
// when the final subscription for its entry target disappears.
DARTPLANT_EXPORT DartPlantStatus dartplant_unhook_handle(DartPlantHookHandle* handle);
DARTPLANT_EXPORT uint8_t dartplant_hook_handle_is_active(const DartPlantHookHandle* handle);
DARTPLANT_EXPORT uint8_t dartplant_hook_handle_is_idle(const DartPlantHookHandle* handle);
// Installs a read-only notification for Dart exceptions that unwind out of the
// hooked invocation. With an exact VM V3 adapter it may also inspect the active
// exception and stacktrace through dartplant_invocation_get_exception() /
// dartplant_invocation_get_stacktrace(). Those values remain read-only:
// retention, replacement/suppression, and async-frame mutation are not exposed.
DARTPLANT_EXPORT DartPlantStatus dartplant_hook_handle_set_exception_callback(
    DartPlantHookHandle* handle, DartPlantExceptionCallback callback, void* user_data);
// Release after unhook_handle() and is_idle()==1. If an invocation still owns
// the listener snapshot, the handle is retained and last_error is set.
DARTPLANT_EXPORT void dartplant_release_hook_handle(DartPlantHookHandle* handle);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_HOOK_H_
