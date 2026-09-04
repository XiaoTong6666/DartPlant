// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "vm/object_bridge.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#include "core/internal.h"

namespace {

constexpr size_t kVmAdapterCallbacksV1Size =
    offsetof(DartPlantVmAdapterCallbacks, pin_generated_roots);
constexpr size_t kVmAdapterCallbacksV2Size =
    offsetof(DartPlantVmAdapterCallbacks, enter_generated_to_native);

bool SameIsolate(const DartPlantIsolateIdentity& left, const DartPlantIsolateIdentity& right) {
    return left.isolate == right.isolate && left.isolate_group == right.isolate_group &&
           left.generation == right.generation;
}

bool SameOwner(const DartPlantVmAdapter& adapter) {
    return adapter.owner_thread == std::this_thread::get_id();
}

bool ValidAdapterCallbacks(const DartPlantVmAdapterCallbacks& callbacks) {
    return callbacks.struct_size >= kVmAdapterCallbacksV1Size && callbacks.adapter_version != 0 &&
           callbacks.enter_isolate != nullptr && callbacks.leave_isolate != nullptr &&
           callbacks.enter_scope != nullptr && callbacks.leave_scope != nullptr &&
           callbacks.retain_object != nullptr && callbacks.release_object != nullptr &&
           callbacks.object_kind != nullptr && callbacks.object_to_raw != nullptr &&
           callbacks.object_is_alive != nullptr;
}

bool GeneratedRootCallbacksAvailable(const DartPlantVmAdapterCallbacks& callbacks) {
    return callbacks.struct_size >= kVmAdapterCallbacksV2Size &&
           callbacks.pin_generated_roots != nullptr && callbacks.generated_root_get != nullptr &&
           callbacks.generated_root_set != nullptr && callbacks.unpin_generated_roots != nullptr;
}

bool GeneratedTransitionCallbacksAvailable(const DartPlantVmAdapterCallbacks& callbacks) {
    return callbacks.struct_size >= kVmAdapterCallbacksV2Size +
                                        sizeof(DartPlantEnterGeneratedToNativeCallback) +
                                        sizeof(DartPlantLeaveNativeToGeneratedCallback) &&
           callbacks.enter_generated_to_native != nullptr &&
           callbacks.leave_native_to_generated != nullptr;
}

bool ActiveObjectCallbacksAvailable(const DartPlantVmAdapterCallbacks& callbacks) {
    return callbacks.struct_size >= offsetof(DartPlantVmAdapterCallbacks, read_active_stacktrace) +
                                        sizeof(DartPlantReadActiveObjectCallback) &&
           callbacks.read_active_exception != nullptr &&
           callbacks.read_active_stacktrace != nullptr;
}

bool TypeArgumentsElementCallbackAvailable(const DartPlantVmAdapterCallbacks& callbacks) {
    return callbacks.struct_size >=
               offsetof(DartPlantVmAdapterCallbacks, read_type_arguments_element) +
                   sizeof(DartPlantReadTypeArgumentsElementCallback) &&
           callbacks.read_type_arguments_element != nullptr;
}

DartPlantStatus CheckAttachedOwnerLocked(DartPlantVmAdapter* adapter) {
    if (adapter == nullptr || !adapter->attached) {
        dartplant::SetLastError("VM adapter has no attached isolate");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (adapter->owner_thread != std::thread::id{} && !SameOwner(*adapter)) {
        dartplant::SetLastError("VM adapter is owned by another thread");
        return DARTPLANT_VM_THREAD_MISMATCH;
    }
    if (adapter->owner_thread == std::thread::id{}) {
        adapter->owner_thread = std::this_thread::get_id();
    }
    return DARTPLANT_OK;
}

DartPlantStatus CheckEnteredLocked(const DartPlantVmAdapter& adapter) {
    if (!adapter.attached) {
        dartplant::SetLastError("VM adapter has no attached isolate");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (adapter.entered == 0) {
        dartplant::SetLastError("VM adapter scope is not entered");
        return DARTPLANT_VM_SCOPE_REQUIRED;
    }
    if (!SameOwner(adapter)) {
        dartplant::SetLastError("VM adapter is owned by another thread");
        return DARTPLANT_VM_THREAD_MISMATCH;
    }
    return DARTPLANT_OK;
}

}  // namespace

namespace dartplant {

bool VmAdapterIsEntered(DartPlantVmAdapter* adapter) {
    if (adapter == nullptr) return false;
    std::lock_guard lock(adapter->mutex);
    return CheckEnteredLocked(*adapter) == DARTPLANT_OK;
}

bool VmAdapterSupportsGeneratedRootBridge(const DartPlantVmAdapter* adapter) {
    return adapter != nullptr && GeneratedRootCallbacksAvailable(adapter->callbacks);
}

bool VmAdapterSupportsGeneratedCallbackBridge(const DartPlantVmAdapter* adapter) {
    return adapter != nullptr && GeneratedRootCallbacksAvailable(adapter->callbacks) &&
           GeneratedTransitionCallbacksAvailable(adapter->callbacks);
}

bool VmAdapterSupportsTypeArgumentsElementRead(const DartPlantVmAdapter* adapter) {
    return adapter != nullptr && TypeArgumentsElementCallbackAvailable(adapter->callbacks);
}

void VmAdapterRetainHook(DartPlantVmAdapter* adapter) {
    if (adapter == nullptr) return;
    std::lock_guard lock(adapter->mutex);
    ++adapter->hook_refs;
}

void VmAdapterReleaseHook(DartPlantVmAdapter* adapter) {
    if (adapter == nullptr) return;
    std::lock_guard lock(adapter->mutex);
    if (adapter->hook_refs != 0) --adapter->hook_refs;
}

DartPlantStatus VmAdapterPinGeneratedRoots(DartPlantVmAdapter* adapter, const uint64_t* raw_values,
                                           uint32_t value_count, void** out_root_lease) {
    if (adapter == nullptr || raw_values == nullptr || value_count == 0 ||
        out_root_lease == nullptr) {
        SetLastError("generated root pin arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    *out_root_lease = nullptr;
    std::unique_lock lock(adapter->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        SetLastError("VM adapter generated-root state is busy");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    if (!GeneratedRootCallbacksAvailable(adapter->callbacks)) {
        SetLastError("VM adapter has no generated-root bridge");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const DartPlantStatus owner = CheckAttachedOwnerLocked(adapter);
    if (owner != DARTPLANT_OK) return owner;
    if (adapter->generated_native_transitions != 0 || adapter->entered != 0) {
        SetLastError("generated roots must be pinned while the mutator is in generated state");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    void* lease = nullptr;
    const DartPlantStatus status = adapter->callbacks.pin_generated_roots(
        adapter->user_data, &adapter->isolate, raw_values, value_count, &lease);
    if (status != DARTPLANT_OK) return status;
    if (lease == nullptr) {
        SetLastError("VM adapter returned a null generated-root lease");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    ++adapter->generated_root_leases;
    *out_root_lease = lease;
    return DARTPLANT_OK;
}

DartPlantStatus VmAdapterGeneratedRootGet(DartPlantVmAdapter* adapter, void* root_lease,
                                          uint32_t index, uint64_t* out_raw) {
    if (adapter == nullptr || root_lease == nullptr || out_raw == nullptr) {
        SetLastError("generated root read arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(adapter->mutex);
    if (!GeneratedRootCallbacksAvailable(adapter->callbacks) ||
        adapter->generated_root_leases == 0) {
        SetLastError("generated root lease is unavailable");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const DartPlantStatus entered = CheckEnteredLocked(*adapter);
    if (entered != DARTPLANT_OK) return entered;
    return adapter->callbacks.generated_root_get(adapter->user_data, &adapter->isolate, root_lease,
                                                 index, out_raw);
}

DartPlantStatus VmAdapterGeneratedRootSet(DartPlantVmAdapter* adapter, void* root_lease,
                                          uint32_t index, uint64_t raw) {
    if (adapter == nullptr || root_lease == nullptr) {
        SetLastError("generated root write arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(adapter->mutex);
    if (!GeneratedRootCallbacksAvailable(adapter->callbacks) ||
        adapter->generated_root_leases == 0) {
        SetLastError("generated root lease is unavailable");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const DartPlantStatus entered = CheckEnteredLocked(*adapter);
    if (entered != DARTPLANT_OK) return entered;
    return adapter->callbacks.generated_root_set(adapter->user_data, &adapter->isolate, root_lease,
                                                 index, raw);
}

DartPlantStatus VmAdapterUnpinGeneratedRoots(DartPlantVmAdapter* adapter, void* root_lease,
                                             uint64_t* out_raw_values, uint32_t value_count) {
    if (adapter == nullptr || root_lease == nullptr || out_raw_values == nullptr ||
        value_count == 0) {
        SetLastError("generated root unpin arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::unique_lock lock(adapter->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        SetLastError("VM adapter generated/native transition state is busy");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    if (!GeneratedRootCallbacksAvailable(adapter->callbacks) ||
        adapter->generated_root_leases == 0) {
        SetLastError("generated root lease is unavailable");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const DartPlantStatus owner = CheckAttachedOwnerLocked(adapter);
    if (owner != DARTPLANT_OK) return owner;
    if (adapter->generated_native_transitions != 0 || adapter->entered != 0) {
        SetLastError("generated roots can only be unpinned after returning to generated state");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    const DartPlantStatus status = adapter->callbacks.unpin_generated_roots(
        adapter->user_data, &adapter->isolate, root_lease, out_raw_values, value_count);
    if (status != DARTPLANT_OK) return status;
    --adapter->generated_root_leases;
    if (adapter->generated_root_leases == 0 && !adapter->isolate_entered) {
        adapter->owner_thread = {};
    }
    return DARTPLANT_OK;
}

DartPlantStatus VmAdapterEnterGeneratedToNative(DartPlantVmAdapter* adapter,
                                                const DartPlantGeneratedTransitionFrame* frame,
                                                void* root_lease) {
    if (adapter == nullptr || frame == nullptr ||
        frame->struct_size < sizeof(DartPlantGeneratedTransitionFrame) || root_lease == nullptr) {
        SetLastError("generated-to-native transition arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::unique_lock lock(adapter->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        SetLastError("VM adapter generated/native transition state is busy");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    if (!GeneratedTransitionCallbacksAvailable(adapter->callbacks) ||
        adapter->generated_root_leases == 0) {
        SetLastError("VM adapter has no complete generated callback bridge");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const DartPlantStatus owner = CheckAttachedOwnerLocked(adapter);
    if (owner != DARTPLANT_OK) return owner;
    if (adapter->generated_native_transitions != 0 || adapter->entered != 0) {
        SetLastError("VM adapter already has a generated/native transition or scope");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    const DartPlantStatus status = adapter->callbacks.enter_generated_to_native(
        adapter->user_data, &adapter->isolate, frame, root_lease);
    if (status != DARTPLANT_OK) return status;
    ++adapter->generated_native_transitions;
    return DARTPLANT_OK;
}

DartPlantStatus VmAdapterLeaveNativeToGenerated(DartPlantVmAdapter* adapter,
                                                const DartPlantGeneratedTransitionFrame* frame,
                                                void* root_lease) {
    if (adapter == nullptr || frame == nullptr ||
        frame->struct_size < sizeof(DartPlantGeneratedTransitionFrame) || root_lease == nullptr) {
        SetLastError("native-to-generated transition arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(adapter->mutex);
    if (!GeneratedTransitionCallbacksAvailable(adapter->callbacks) ||
        adapter->generated_native_transitions == 0 || adapter->generated_root_leases == 0) {
        SetLastError("VM adapter generated/native transition is not active");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (!SameOwner(*adapter)) {
        SetLastError("VM adapter is owned by another thread");
        return DARTPLANT_VM_THREAD_MISMATCH;
    }
    if (adapter->entered != 0) {
        SetLastError("VM adapter scope must be left before returning to generated state");
        return DARTPLANT_VM_SCOPE_REQUIRED;
    }
    const DartPlantStatus status = adapter->callbacks.leave_native_to_generated(
        adapter->user_data, &adapter->isolate, frame, root_lease);
    if (status != DARTPLANT_OK) return status;
    --adapter->generated_native_transitions;
    return DARTPLANT_OK;
}

DartPlantStatus ReadActiveObject(DartPlantVmAdapter* adapter,
                                 DartPlantReadActiveObjectCallback callback, uint64_t* out_raw) {
    if (adapter == nullptr || out_raw == nullptr) {
        SetLastError("active object read arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(adapter->mutex);
    if (!ActiveObjectCallbacksAvailable(adapter->callbacks)) {
        SetLastError("VM adapter has no active exception object bridge");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (!adapter->attached) {
        SetLastError("VM adapter has no attached isolate");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (!SameOwner(*adapter)) {
        SetLastError("VM adapter is owned by another thread");
        return DARTPLANT_VM_THREAD_MISMATCH;
    }
    return callback(adapter->user_data, &adapter->isolate, out_raw);
}

DartPlantStatus VmAdapterReadActiveException(DartPlantVmAdapter* adapter, uint64_t* out_raw) {
    return ReadActiveObject(
        adapter, adapter == nullptr ? nullptr : adapter->callbacks.read_active_exception, out_raw);
}

DartPlantStatus VmAdapterReadActiveStacktrace(DartPlantVmAdapter* adapter, uint64_t* out_raw) {
    return ReadActiveObject(
        adapter, adapter == nullptr ? nullptr : adapter->callbacks.read_active_stacktrace, out_raw);
}

DartPlantStatus VmAdapterReadTypeArgumentsElementGenerated(DartPlantVmAdapter* adapter,
                                                           uint64_t type_arguments_raw,
                                                           uint32_t index, uint64_t* out_raw) {
    if (adapter == nullptr || out_raw == nullptr) {
        SetLastError("TypeArguments element read arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::unique_lock lock(adapter->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        SetLastError("VM adapter TypeArguments state is busy");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    if (!TypeArgumentsElementCallbackAvailable(adapter->callbacks)) {
        SetLastError("VM adapter has no TypeArguments element bridge");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const DartPlantStatus owner = CheckAttachedOwnerLocked(adapter);
    if (owner != DARTPLANT_OK) return owner;
    if (adapter->generated_native_transitions != 0 || adapter->entered != 0) {
        SetLastError("TypeArguments elements must be captured while Dart is still generated");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    return adapter->callbacks.read_type_arguments_element(adapter->user_data, &adapter->isolate,
                                                          type_arguments_raw, index, out_raw);
}

DartPlantStatus VmAdapterCheckHandle(const DartPlantVmAdapter* adapter,
                                     const DartPlantObjectHandle* handle) {
    if (adapter == nullptr || handle == nullptr || handle->adapter != adapter || handle->released ||
        handle->adapter_version != adapter->callbacks.adapter_version) {
        SetLastError("object handle is invalid");
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    std::lock_guard lock(adapter->mutex);
    if (!SameIsolate(adapter->isolate, handle->isolate)) {
        SetLastError("object handle belongs to another isolate generation");
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    return CheckEnteredLocked(*adapter);
}

DartPlantStatus VmAdapterRetainObject(DartPlantVmAdapter* adapter, uint64_t raw,
                                      DartPlantObjectStrength strength,
                                      DartPlantObjectHandle** out_handle) {
    if (adapter == nullptr || out_handle == nullptr) {
        SetLastError("VM object retain arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;
    std::lock_guard lock(adapter->mutex);
    const DartPlantStatus entered = CheckEnteredLocked(*adapter);
    if (entered != DARTPLANT_OK) return entered;

    void* backend_handle = nullptr;
    const DartPlantStatus retained = adapter->callbacks.retain_object(
        adapter->user_data, &adapter->isolate, raw, strength, &backend_handle);
    if (retained != DARTPLANT_OK || backend_handle == nullptr) {
        if (retained == DARTPLANT_OK) SetLastError("VM adapter returned a null object handle");
        return retained == DARTPLANT_OK ? DARTPLANT_OBJECT_HANDLE_INVALID : retained;
    }

    DartPlantObjectKind kind = DARTPLANT_OBJECT_UNKNOWN;
    const DartPlantStatus classified = adapter->callbacks.object_kind(
        adapter->user_data, &adapter->isolate, backend_handle, &kind);
    if (classified != DARTPLANT_OK) {
        adapter->callbacks.release_object(adapter->user_data, &adapter->isolate, backend_handle,
                                          strength);
        return classified;
    }

    auto* handle = new DartPlantObjectHandle;
    handle->adapter = adapter;
    handle->adapter_version = adapter->callbacks.adapter_version;
    handle->isolate = adapter->isolate;
    handle->strength = strength;
    handle->kind = kind;
    handle->backend_handle = backend_handle;
    ++adapter->live_handles;
    *out_handle = handle;
    return DARTPLANT_OK;
}

DartPlantStatus VmAdapterSetRaw(const DartPlantObjectHandle* handle, uint64_t* out_raw) {
    if (handle == nullptr || out_raw == nullptr || handle->adapter == nullptr || handle->released) {
        SetLastError("object handle is invalid");
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    DartPlantVmAdapter* adapter = handle->adapter;
    std::lock_guard lock(adapter->mutex);
    const DartPlantStatus entered = CheckEnteredLocked(*adapter);
    if (entered != DARTPLANT_OK) return entered;
    if (!SameIsolate(adapter->isolate, handle->isolate)) {
        SetLastError("object handle belongs to another isolate generation");
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    return adapter->callbacks.object_to_raw(adapter->user_data, &adapter->isolate,
                                            handle->backend_handle, out_raw);
}

}  // namespace dartplant

extern "C" {

DARTPLANT_EXPORT DartPlantStatus
dartplant_vm_adapter_create(const DartPlantVmAdapterCallbacks* callbacks, void* user_data,
                            DartPlantVmAdapter** out_adapter) {
    if (callbacks == nullptr || out_adapter == nullptr || !ValidAdapterCallbacks(*callbacks)) {
        dartplant::SetLastError("VM adapter callbacks are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto* adapter = new DartPlantVmAdapter;
    std::memset(&adapter->callbacks, 0, sizeof(adapter->callbacks));
    std::memcpy(&adapter->callbacks, callbacks,
                std::min<size_t>(callbacks->struct_size, sizeof(adapter->callbacks)));
    adapter->user_data = user_data;
    *out_adapter = adapter;
    return DARTPLANT_OK;
}

DARTPLANT_EXPORT DartPlantStatus dartplant_vm_adapter_destroy(DartPlantVmAdapter* adapter) {
    if (adapter == nullptr) {
        dartplant::SetLastError("VM adapter is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::unique_lock lock(adapter->mutex);
    if (adapter->attached || adapter->entered != 0 || adapter->isolate_entered ||
        adapter->live_handles != 0 || adapter->hook_refs != 0 ||
        adapter->generated_root_leases != 0 || adapter->generated_native_transitions != 0) {
        dartplant::SetLastError("VM adapter still owns an isolate, scope, or object handle");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    lock.unlock();
    for (DartPlantObjectHandle* handle : adapter->released_handles) delete handle;
    delete adapter;
    return DARTPLANT_OK;
}

DARTPLANT_EXPORT DartPlantStatus dartplant_vm_adapter_attach_isolate(
    DartPlantVmAdapter* adapter, const DartPlantIsolateIdentity* isolate) {
    if (adapter == nullptr || isolate == nullptr) {
        dartplant::SetLastError("VM adapter isolate arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(adapter->mutex);
    if (adapter->attached || adapter->entered != 0) {
        dartplant::SetLastError("VM adapter already has an isolate");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    adapter->isolate = *isolate;
    adapter->attached = true;
    return DARTPLANT_OK;
}

DARTPLANT_EXPORT DartPlantStatus dartplant_vm_adapter_detach_isolate(
    DartPlantVmAdapter* adapter, const DartPlantIsolateIdentity* isolate) {
    if (adapter == nullptr || isolate == nullptr) {
        dartplant::SetLastError("VM adapter isolate arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(adapter->mutex);
    if (!adapter->attached || !SameIsolate(adapter->isolate, *isolate)) {
        dartplant::SetLastError("VM adapter isolate does not match");
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    if (adapter->entered != 0 || adapter->isolate_entered || adapter->live_handles != 0 ||
        adapter->generated_root_leases != 0 || adapter->generated_native_transitions != 0) {
        dartplant::SetLastError("VM adapter isolate still has active scopes or handles");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    adapter->attached = false;
    adapter->isolate = {};
    adapter->owner_thread = {};
    return DARTPLANT_OK;
}

DARTPLANT_EXPORT DartPlantStatus dartplant_vm_enter_isolate(DartPlantVmAdapter* adapter) {
    if (adapter == nullptr) {
        dartplant::SetLastError("VM adapter is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(adapter->mutex);
    if (!adapter->attached) {
        dartplant::SetLastError("VM adapter has no attached isolate");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (adapter->entered != 0 || adapter->isolate_entered) {
        dartplant::SetLastError("VM adapter already has an entered isolate or scope");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    if (adapter->owner_thread != std::thread::id{} && !SameOwner(*adapter)) {
        dartplant::SetLastError("VM adapter is owned by another thread");
        return DARTPLANT_VM_THREAD_MISMATCH;
    }
    adapter->owner_thread = std::this_thread::get_id();
    const DartPlantStatus status =
        adapter->callbacks.enter_isolate(adapter->user_data, &adapter->isolate);
    if (status != DARTPLANT_OK) return status;
    adapter->isolate_entered = true;
    return DARTPLANT_OK;
}

DARTPLANT_EXPORT DartPlantStatus dartplant_vm_leave_isolate(DartPlantVmAdapter* adapter) {
    if (adapter == nullptr) {
        dartplant::SetLastError("VM adapter is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(adapter->mutex);
    if (!adapter->isolate_entered || adapter->entered != 0 || !SameOwner(*adapter)) {
        dartplant::SetLastError("VM isolate cannot be left while scopes are active");
        return adapter->entered != 0 ? DARTPLANT_VM_SCOPE_REQUIRED : DARTPLANT_VM_THREAD_MISMATCH;
    }
    const DartPlantStatus isolate_status =
        adapter->callbacks.leave_isolate(adapter->user_data, &adapter->isolate);
    if (isolate_status != DARTPLANT_OK) return isolate_status;
    adapter->isolate_entered = false;
    adapter->owner_thread = {};
    return DARTPLANT_OK;
}

DARTPLANT_EXPORT DartPlantStatus dartplant_vm_enter_scope(DartPlantVmAdapter* adapter) {
    if (adapter == nullptr) {
        dartplant::SetLastError("VM adapter is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(adapter->mutex);
    if (!adapter->attached) {
        dartplant::SetLastError("VM adapter has no attached isolate");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (adapter->entered != 0 && !SameOwner(*adapter)) {
        dartplant::SetLastError("VM adapter is owned by another thread");
        return DARTPLANT_VM_THREAD_MISMATCH;
    }
    if (adapter->entered == 0) adapter->owner_thread = std::this_thread::get_id();
    const DartPlantStatus status =
        adapter->callbacks.enter_scope(adapter->user_data, &adapter->isolate);
    if (status != DARTPLANT_OK) return status;
    ++adapter->entered;
    return DARTPLANT_OK;
}

DARTPLANT_EXPORT DartPlantStatus dartplant_vm_leave_scope(DartPlantVmAdapter* adapter) {
    if (adapter == nullptr) {
        dartplant::SetLastError("VM adapter is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(adapter->mutex);
    const DartPlantStatus entered = CheckEnteredLocked(*adapter);
    if (entered != DARTPLANT_OK) return entered;
    const DartPlantStatus status =
        adapter->callbacks.leave_scope(adapter->user_data, &adapter->isolate);
    if (status != DARTPLANT_OK) return status;
    --adapter->entered;
    if (adapter->entered == 0 && !adapter->isolate_entered && adapter->generated_root_leases == 0 &&
        adapter->generated_native_transitions == 0) {
        adapter->owner_thread = {};
    }
    return DARTPLANT_OK;
}

DARTPLANT_EXPORT DartPlantStatus dartplant_object_retain(DartPlantVmAdapter* adapter, uint64_t raw,
                                                         DartPlantObjectStrength strength,
                                                         DartPlantObjectHandle** out_handle) {
    return dartplant::VmAdapterRetainObject(adapter, raw, strength, out_handle);
}

DARTPLANT_EXPORT DartPlantStatus dartplant_object_release(DartPlantObjectHandle* handle) {
    if (handle == nullptr || handle->adapter == nullptr || handle->released) {
        dartplant::SetLastError("object handle is invalid");
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    DartPlantVmAdapter* adapter = handle->adapter;
    std::lock_guard lock(adapter->mutex);
    if (!adapter->attached || !SameIsolate(adapter->isolate, handle->isolate)) {
        dartplant::SetLastError("object handle belongs to another isolate generation");
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    const DartPlantStatus status = adapter->callbacks.release_object(
        adapter->user_data, &adapter->isolate, handle->backend_handle, handle->strength);
    if (status != DARTPLANT_OK) return status;
    handle->released = true;
    handle->backend_handle = nullptr;
    if (adapter->live_handles != 0) --adapter->live_handles;
    adapter->released_handles.push_back(handle);
    return DARTPLANT_OK;
}

DARTPLANT_EXPORT DartPlantStatus dartplant_object_kind(const DartPlantObjectHandle* handle,
                                                       DartPlantObjectKind* out_kind) {
    if (handle == nullptr || out_kind == nullptr || handle->released) {
        dartplant::SetLastError("object handle is invalid");
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    *out_kind = handle->kind;
    return DARTPLANT_OK;
}

DARTPLANT_EXPORT DartPlantStatus dartplant_object_to_raw(const DartPlantObjectHandle* handle,
                                                         uint64_t* out_raw) {
    return dartplant::VmAdapterSetRaw(handle, out_raw);
}

DARTPLANT_EXPORT DartPlantStatus dartplant_object_is_alive(const DartPlantObjectHandle* handle,
                                                           uint8_t* out_alive) {
    if (handle == nullptr || out_alive == nullptr || handle->adapter == nullptr ||
        handle->released) {
        dartplant::SetLastError("object handle is invalid");
        return DARTPLANT_OBJECT_HANDLE_INVALID;
    }
    DartPlantVmAdapter* adapter = handle->adapter;
    std::lock_guard lock(adapter->mutex);
    const DartPlantStatus entered = CheckEnteredLocked(*adapter);
    if (entered != DARTPLANT_OK) return entered;
    if (!SameIsolate(adapter->isolate, handle->isolate)) {
        dartplant::SetLastError("object handle belongs to another isolate generation");
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    return adapter->callbacks.object_is_alive(adapter->user_data, &adapter->isolate,
                                              handle->backend_handle, out_alive);
}

}  // extern "C"
