// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>

#include "runtime/default_runtime.h"
#include "runtime/runtime_internal.h"

extern "C" DartPlantStatus dartplant_runtime_hook_method_raw(DartPlantRuntime* runtime,
                                                             const DartPlantMethod* method,
                                                             void* replacement, void** backup,
                                                             DartPlantHook** out_hook) {
    if (runtime == nullptr || method == nullptr || method->function == nullptr ||
        method->function->code_target == nullptr) {
        dartplant::SetLastError("runtime method has no code target");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    if (!dartplant::RuntimeReadyForMethodOperation(runtime, method)) {
        dartplant::SetLastError("runtime is not ready for method hooks");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!runtime->profile_matched) {
        dartplant::SetLastError("runtime app image is not matched");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (!dartplant::IsCurrentRuntimeMethod(runtime, method)) {
        dartplant::SetLastError("method belongs to a stale or different runtime generation");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const DartPlantStatus status =
        dartplant::InstallHook(method->function->code_target, replacement, backup, out_hook);
    if (status == DARTPLANT_OK) {
        (*out_hook)->runtime_generation = runtime->generation;
        (*out_hook)->expected_runtime_generation =
            runtime->generation->load(std::memory_order_acquire);
    }
    return status;
}

extern "C" DartPlantStatus dartplant_runtime_hook_method(DartPlantRuntime* runtime,
                                                         const DartPlantMethod* method,
                                                         const DartPlantHookOptions* options,
                                                         DartPlantHook** out_hook) {
    if (runtime == nullptr || method == nullptr || options == nullptr || out_hook == nullptr) {
        dartplant::SetLastError("runtime callback hook arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    if (!dartplant::RuntimeReadyForMethodOperation(runtime, method)) {
        dartplant::SetLastError("runtime is not ready for method hooks");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!runtime->profile_matched) {
        dartplant::SetLastError("runtime app image is not matched");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (!dartplant::IsCurrentRuntimeMethod(runtime, method)) {
        dartplant::SetLastError("method belongs to a stale or different runtime generation");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const uint64_t generation = runtime->generation->load(std::memory_order_acquire);
    const auto call_layout = dartplant::FindRuntimeCallLayoutLocked(runtime, method);
    return dartplant::InstallCallbackHook(method, runtime->profile.profile, *options, 0, out_hook,
                                          nullptr, runtime->live_vm_null_value, runtime->generation,
                                          generation, runtime->live_vm_bool_true_value,
                                          runtime->live_vm_bool_false_value, call_layout);
}

extern "C" DartPlantStatus dartplant_runtime_hook_method_handle(DartPlantRuntime* runtime,
                                                                const DartPlantMethod* method,
                                                                const DartPlantHookOptions* options,
                                                                DartPlantHookHandle** out_handle) {
    if (runtime == nullptr || method == nullptr || options == nullptr || out_handle == nullptr) {
        dartplant::SetLastError("runtime logical hook arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const DartPlantStatus evidence_status =
        dartplant::BindRegisteredCompilerEvidenceIfPresent(runtime, method);
    if (evidence_status != DARTPLANT_OK) return evidence_status;
    DartPlantListener* listener = nullptr;
    const DartPlantStatus status =
        dartplant_runtime_add_listener(runtime, method, options, 0, &listener);
    if (status != DARTPLANT_OK) return status;
    auto* handle = new DartPlantHookHandle;
    handle->listener = listener;
    *out_handle = handle;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_runtime_hook_method_with_profile(
    DartPlantRuntime* runtime, const DartPlantMethod* method,
    const DartPlantRuntimeProfile* profile, const DartPlantHookOptions* options,
    DartPlantHook** out_hook) {
    if (runtime == nullptr || method == nullptr || profile == nullptr || options == nullptr ||
        out_hook == nullptr) {
        dartplant::SetLastError("runtime profile hook arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    if (!dartplant::RuntimeReadyForMethodOperation(runtime, method)) {
        dartplant::SetLastError("runtime is not ready for method hooks");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!runtime->profile_matched) {
        dartplant::SetLastError("runtime app image is not matched");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (!dartplant::IsCurrentRuntimeMethod(runtime, method)) {
        dartplant::SetLastError("method belongs to a stale or different runtime generation");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const uint64_t generation = runtime->generation->load(std::memory_order_acquire);
    return dartplant::InstallCallbackHook(method, *profile, *options, 0, out_hook, nullptr,
                                          runtime->live_vm_null_value, runtime->generation,
                                          generation, runtime->live_vm_bool_true_value,
                                          runtime->live_vm_bool_false_value);
}

extern "C" DartPlantStatus dartplant_runtime_add_listener(DartPlantRuntime* runtime,
                                                          const DartPlantMethod* method,
                                                          const DartPlantHookOptions* options,
                                                          int32_t priority,
                                                          DartPlantListener** out_listener) {
    if (runtime == nullptr || method == nullptr || options == nullptr || out_listener == nullptr) {
        dartplant::SetLastError("runtime listener arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    std::lock_guard lock(runtime->mutex);
    if (!dartplant::RuntimeReadyForMethodOperation(runtime, method)) {
        dartplant::SetLastError("runtime is not ready for method listeners");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!runtime->profile_matched) {
        dartplant::SetLastError("runtime app image is not matched");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (!dartplant::IsCurrentRuntimeMethod(runtime, method)) {
        dartplant::SetLastError("method belongs to a stale or different runtime generation");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const uint64_t generation = runtime->generation->load(std::memory_order_acquire);

    DartPlantStatus status = dartplant::AddCallbackListenerForMethod(
        method, *options, priority, out_listener, runtime->generation, generation);
    if (status == DARTPLANT_NOT_INITIALIZED) {
        const auto call_layout = dartplant::FindRuntimeCallLayoutLocked(runtime, method);
        return dartplant::InstallCallbackHook(
            method, runtime->profile.profile, *options, priority, nullptr, out_listener,
            runtime->live_vm_null_value, runtime->generation, generation,
            runtime->live_vm_bool_true_value, runtime->live_vm_bool_false_value, call_layout);
    }
    return status;
}

extern "C" DartPlantStatus dartplant_remove_listener(DartPlantListener* listener) {
    if (listener == nullptr || listener->record == nullptr || listener->hook == nullptr) {
        dartplant::SetLastError("listener is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    DartPlantHook* hook = listener->hook;
    bool remove_hook = false;
    {
        std::lock_guard lock(hook->mutex);
        listener->record->active.store(false, std::memory_order_release);
        hook->listeners.erase(
            std::remove_if(hook->listeners.begin(), hook->listeners.end(),
                           [listener](const auto& record) { return record == listener->record; }),
            hook->listeners.end());
        remove_hook =
            hook->listeners.empty() && hook->state == dartplant::HookRecordState::kInstalled;
    }
    return remove_hook ? dartplant::RemoveHook(hook) : DARTPLANT_OK;
}

extern "C" void dartplant_release_listener(DartPlantListener* listener) {
    if (listener == nullptr) return;
    if (listener->record != nullptr &&
        listener->record->in_flight.load(std::memory_order_acquire) != 0) {
        dartplant::SetLastError(
            "listener is still referenced by an in-flight invocation; remove and wait for idle");
        return;
    }
    dartplant_remove_listener(listener);
    if (listener->hook != nullptr) {
        std::lock_guard lock(listener->hook->mutex);
        if (listener->hook->listener_handles != 0) {
            --listener->hook->listener_handles;
        }
    }
    delete listener;
}

extern "C" uint8_t dartplant_listener_is_active(const DartPlantListener* listener) {
    return listener != nullptr && listener->record != nullptr &&
                   listener->record->active.load(std::memory_order_acquire)
               ? 1
               : 0;
}

extern "C" uint8_t dartplant_listener_is_idle(const DartPlantListener* listener) {
    return listener != nullptr && listener->record != nullptr &&
                   listener->record->in_flight.load(std::memory_order_acquire) == 0
               ? 1
               : 0;
}
