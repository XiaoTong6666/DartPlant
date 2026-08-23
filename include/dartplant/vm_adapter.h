// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_VM_ADAPTER_H_
#define DARTPLANT_VM_ADAPTER_H_

#include <stdint.h>

#include "dartplant/dartplant.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DartPlantVmAdapter DartPlantVmAdapter;
typedef struct DartPlantObjectHandle DartPlantObjectHandle;

typedef struct DartPlantIsolateIdentity {
    uint64_t isolate;
    uint64_t isolate_group;
    uint64_t generation;
} DartPlantIsolateIdentity;

typedef enum DartPlantObjectStrength {
    DARTPLANT_OBJECT_STRONG = 0,
    DARTPLANT_OBJECT_WEAK,
} DartPlantObjectStrength;

typedef enum DartPlantObjectKind {
    DARTPLANT_OBJECT_UNKNOWN = 0,
    DARTPLANT_OBJECT_NULL,
    DARTPLANT_OBJECT_BOOL,
    DARTPLANT_OBJECT_SMI,
    DARTPLANT_OBJECT_DOUBLE,
    DARTPLANT_OBJECT_STRING,
    DARTPLANT_OBJECT_LIST,
    DARTPLANT_OBJECT_MAP,
    DARTPLANT_OBJECT_CLOSURE,
    DARTPLANT_OBJECT_OTHER,
} DartPlantObjectKind;

typedef struct DartPlantVmAdapterCallbacks {
    uint32_t struct_size;
    uint32_t adapter_version;
    // These callbacks implement Dart_EnterIsolate/Dart_ExitIsolate.
    // They are used only by explicit external isolate ownership.
    DartPlantStatus (*enter_isolate)(void* user_data, const DartPlantIsolateIdentity* isolate);
    DartPlantStatus (*leave_isolate)(void* user_data, const DartPlantIsolateIdentity* isolate);
    // These callbacks implement Dart_EnterScope/Dart_ExitScope. Hook callbacks
    // use this pair because Dart_EnterIsolate is invalid when an isolate is
    // already current on the mutator thread.
    DartPlantStatus (*enter_scope)(void* user_data, const DartPlantIsolateIdentity* isolate);
    DartPlantStatus (*leave_scope)(void* user_data, const DartPlantIsolateIdentity* isolate);
    DartPlantStatus (*retain_object)(void* user_data, const DartPlantIsolateIdentity* isolate,
                                     uint64_t raw, DartPlantObjectStrength strength,
                                     void** out_backend_handle);
    DartPlantStatus (*release_object)(void* user_data, const DartPlantIsolateIdentity* isolate,
                                      void* backend_handle, DartPlantObjectStrength strength);
    DartPlantStatus (*object_kind)(void* user_data, const DartPlantIsolateIdentity* isolate,
                                   void* backend_handle, DartPlantObjectKind* out_kind);
    DartPlantStatus (*object_to_raw)(void* user_data, const DartPlantIsolateIdentity* isolate,
                                     void* backend_handle, uint64_t* out_raw);
    DartPlantStatus (*object_is_alive)(void* user_data, const DartPlantIsolateIdentity* isolate,
                                       void* backend_handle, uint8_t* out_alive);
} DartPlantVmAdapterCallbacks;

DARTPLANT_EXPORT DartPlantStatus
dartplant_vm_adapter_create(const DartPlantVmAdapterCallbacks* callbacks, void* user_data,
                            DartPlantVmAdapter** out_adapter);
DARTPLANT_EXPORT DartPlantStatus dartplant_vm_adapter_destroy(DartPlantVmAdapter* adapter);

DARTPLANT_EXPORT DartPlantStatus dartplant_vm_adapter_attach_isolate(
    DartPlantVmAdapter* adapter, const DartPlantIsolateIdentity* isolate);
DARTPLANT_EXPORT DartPlantStatus dartplant_vm_adapter_detach_isolate(
    DartPlantVmAdapter* adapter, const DartPlantIsolateIdentity* isolate);
DARTPLANT_EXPORT DartPlantStatus dartplant_vm_enter_isolate(DartPlantVmAdapter* adapter);
DARTPLANT_EXPORT DartPlantStatus dartplant_vm_leave_isolate(DartPlantVmAdapter* adapter);
DARTPLANT_EXPORT DartPlantStatus dartplant_vm_enter_scope(DartPlantVmAdapter* adapter);
DARTPLANT_EXPORT DartPlantStatus dartplant_vm_leave_scope(DartPlantVmAdapter* adapter);

DARTPLANT_EXPORT DartPlantStatus dartplant_object_retain(DartPlantVmAdapter* adapter, uint64_t raw,
                                                         DartPlantObjectStrength strength,
                                                         DartPlantObjectHandle** out_handle);
DARTPLANT_EXPORT DartPlantStatus dartplant_object_release(DartPlantObjectHandle* handle);
DARTPLANT_EXPORT DartPlantStatus dartplant_object_kind(const DartPlantObjectHandle* handle,
                                                       DartPlantObjectKind* out_kind);
DARTPLANT_EXPORT DartPlantStatus dartplant_object_to_raw(const DartPlantObjectHandle* handle,
                                                         uint64_t* out_raw);
DARTPLANT_EXPORT DartPlantStatus dartplant_object_is_alive(const DartPlantObjectHandle* handle,
                                                           uint8_t* out_alive);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_VM_ADAPTER_H_
