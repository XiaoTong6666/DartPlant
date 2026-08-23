// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <utility>

#include "core/internal.h"
#include "runtime/vm_adapter_internal.h"

namespace {

bool SameIsolate(const DartPlantIsolateIdentity& left, const DartPlantIsolateIdentity& right) {
    return left.isolate == right.isolate && left.isolate_group == right.isolate_group &&
           left.generation == right.generation;
}

bool SameOwner(const DartPlantVmAdapter& adapter) {
    return adapter.owner_thread == std::this_thread::get_id();
}

bool ValidAdapterCallbacks(const DartPlantVmAdapterCallbacks& callbacks) {
    return callbacks.struct_size >= sizeof(DartPlantVmAdapterCallbacks) &&
           callbacks.adapter_version != 0 && callbacks.enter_isolate != nullptr &&
           callbacks.leave_isolate != nullptr && callbacks.enter_scope != nullptr &&
           callbacks.leave_scope != nullptr && callbacks.retain_object != nullptr &&
           callbacks.release_object != nullptr && callbacks.object_kind != nullptr &&
           callbacks.object_to_raw != nullptr && callbacks.object_is_alive != nullptr;
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
    adapter->callbacks = *callbacks;
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
        adapter->live_handles != 0 || adapter->hook_refs != 0) {
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
    if (adapter->entered != 0 || adapter->isolate_entered || adapter->live_handles != 0) {
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
    if (adapter->entered == 0 && !adapter->isolate_entered) adapter->owner_thread = {};
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
