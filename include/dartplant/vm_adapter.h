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

// A generated-root lease bridges raw ObjectPtr values that are live in an
// intercepted Dart entry into VM-visible roots before the mutator enters a
// safepoint/native transition. The adapter owns the opaque lease and must keep
// every non-zero slot strongly rooted until unpin_generated_roots().
//
// pin_generated_roots() and unpin_generated_roots() are called while the Dart
// mutator is still in generated state. They must not enter a safepoint, invoke
// Dart API, allocate in the Dart heap, or otherwise permit GC before the root
// set is visible. Implementations typically consume a pool of persistent roots
// preallocated earlier from a normal Dart FFI/native scope.
//
// generated_root_get/set() are called while the callback is inside the
// adapter's native/API scope. They must access the same VM-visible root slots;
// get must return the relocated ObjectPtr after a moving GC.
typedef DartPlantStatus (*DartPlantPinGeneratedRootsCallback)(
    void* user_data, const DartPlantIsolateIdentity* isolate, const uint64_t* raw_values,
    uint32_t value_count, void** out_root_lease);
typedef DartPlantStatus (*DartPlantGeneratedRootGetCallback)(
    void* user_data, const DartPlantIsolateIdentity* isolate, void* root_lease, uint32_t index,
    uint64_t* out_raw);
typedef DartPlantStatus (*DartPlantGeneratedRootSetCallback)(
    void* user_data, const DartPlantIsolateIdentity* isolate, void* root_lease, uint32_t index,
    uint64_t raw);
typedef DartPlantStatus (*DartPlantUnpinGeneratedRootsCallback)(
    void* user_data, const DartPlantIsolateIdentity* isolate, void* root_lease,
    uint64_t* out_raw_values, uint32_t value_count);

typedef struct DartPlantGeneratedTransitionFrame {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t thread;
    uint64_t dart_sp;
    // VM-recognized synthetic ExitFrame FP prepared by DartPlant. On ARM64
    // this frame has saved-caller-FP at slot 0, saved-caller-PC at slot 1 and
    // therefore reports dart_sp as its caller SP. The adapter must publish
    // this value as Thread::top_exit_frame_info before entering a safepoint.
    uint64_t exit_frame;
    uint64_t caller_fp;
    uint64_t caller_lr;
} DartPlantGeneratedTransitionFrame;

typedef enum DartPlantGeneratedTransitionFlags {
    DARTPLANT_GENERATED_TRANSITION_SYNTHETIC_EXIT_FRAME = 1u << 0,
} DartPlantGeneratedTransitionFlags;

// These callbacks perform the VM-specific generated<->native safepoint
// transition around a DartPlant callback. They are intentionally separate from
// Dart_EnterScope/Dart_ExitScope: entering an API scope does not by itself
// change Thread::execution_state, publish an exit frame, or participate in the
// Dart safepoint protocol.
//
// enter_generated_to_native() runs after generated roots have been pinned and
// before enter_scope(). leave_native_to_generated() runs after leave_scope()
// while those roots are still pinned. enter_generated_to_native() must perform
// the same semantic transition as Dart ARM64 TransitionGeneratedToNative:
// publish frame->exit_frame through Thread::top_exit_frame_info, mark an
// appropriate exit-through-native value, switch the VM tag/execution state to
// native and enter the full safepoint protocol. A failed enter callback must
// be failure-atomic and leave the VM in its original generated state. The
// reverse callback must exit that safepoint, restore generated state/Dart VM tag
// and clear the published
// exit-frame state before returning. Once enter_generated_to_native() succeeds,
// leave_native_to_generated() is a non-recoverable transition: if it cannot
// restore generated execution, DartPlant fail-stops instead of resuming Dart
// with a native VM state.
typedef DartPlantStatus (*DartPlantEnterGeneratedToNativeCallback)(
    void* user_data, const DartPlantIsolateIdentity* isolate,
    const DartPlantGeneratedTransitionFrame* frame, void* root_lease);
typedef DartPlantStatus (*DartPlantLeaveNativeToGeneratedCallback)(
    void* user_data, const DartPlantIsolateIdentity* isolate,
    const DartPlantGeneratedTransitionFrame* frame, void* root_lease);

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

    // V2 append-only generated-code callback bridge. All four callbacks are
    // required together before DartPlant permits a real Dart hook to enter a
    // VM/API scope. Older adapters remain valid for external/native use but are
    // deliberately rejected by generated-code hooks.
    DartPlantPinGeneratedRootsCallback pin_generated_roots;
    DartPlantGeneratedRootGetCallback generated_root_get;
    DartPlantGeneratedRootSetCallback generated_root_set;
    DartPlantUnpinGeneratedRootsCallback unpin_generated_roots;

    // V3 append-only generated/native transition contract. A real Dart entry
    // hook requires both V2 root callbacks and this pair before exposing a VM
    // scope to user callbacks.
    DartPlantEnterGeneratedToNativeCallback enter_generated_to_native;
    DartPlantLeaveNativeToGeneratedCallback leave_native_to_generated;
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
