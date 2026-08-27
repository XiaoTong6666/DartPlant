// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>

#include "core/internal.h"

namespace dartplant {
namespace {

std::vector<std::unique_ptr<DartPlantHook>>& Hooks() {
    static std::vector<std::unique_ptr<DartPlantHook>> hooks;
    return hooks;
}

bool IsSupportedArgumentRegister(uint8_t reg) {
    // x16/x17 are veneer scratch registers and x30 carries the continuation.
    return reg < 30 && reg != 16 && reg != 17;
}

bool IsSupportedLocation(const DartPlantAbiLocation& location) {
    if (location.kind == DARTPLANT_ABI_FP_REGISTER) return location.index < 32;
    return IsSupportedArgumentRegister(location.index);
}

bool ValidCallbackOptions(const DartPlantHookOptions& options) {
    constexpr uint32_t kSupportedFlags = DARTPLANT_HOOK_ALLOW_SHARED_CODE;
    return options.struct_size >= sizeof(DartPlantHookOptions) &&
           (options.flags & ~kSupportedFlags) == 0 &&
           (options.on_enter != nullptr || options.on_leave != nullptr);
}

bool AllowsSharedCode(const DartPlantHookOptions& options) {
    return (options.flags & DARTPLANT_HOOK_ALLOW_SHARED_CODE) != 0;
}

std::shared_ptr<DartPlantListenerRecord> MakeListenerLocked(DartPlantHook* hook,
                                                            const DartPlantMethod* requested_method,
                                                            const DartPlantHookOptions& options,
                                                            int32_t priority) {
    auto listener = std::make_shared<DartPlantListenerRecord>();
    listener->id = hook->next_listener_id++;
    listener->priority = priority;
    listener->registration_order = hook->next_registration_order++;
    listener->options = options;
    if (requested_method != nullptr) {
        listener->requested_method = std::make_shared<DartPlantMethod>(*requested_method);
    }
    return listener;
}

void InsertListenerLocked(DartPlantHook* hook, std::shared_ptr<DartPlantListenerRecord> listener) {
    const auto position =
        std::upper_bound(hook->listeners.begin(), hook->listeners.end(), listener,
                         [](const auto& value, const auto& current) {
                             if (value->priority != current->priority) {
                                 return value->priority > current->priority;
                             }
                             return value->registration_order < current->registration_order;
                         });
    hook->listeners.insert(position, std::move(listener));
}

uintptr_t HookTarget(const DartPlantHook* hook) {
    return hook == nullptr || hook->code_target == nullptr ? 0 : hook->code_target->entry;
}

DartPlantHook* FindHookLocked(uintptr_t target) {
    const auto found = std::find_if(Hooks().begin(), Hooks().end(), [target](const auto& hook) {
        std::lock_guard hook_lock(hook->mutex);
        return HookTarget(hook.get()) == target && hook->state != HookRecordState::kUnhooked;
    });
    return found == Hooks().end() ? nullptr : found->get();
}

bool ValidateProfile(const DartPlantRuntimeProfile& profile) {
    constexpr uint32_t kSupportedFlags =
        DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT |
        DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS | DARTPLANT_PROFILE_TAGGED_GP_RESULT;
    if ((profile.flags & ~kSupportedFlags) != 0 ||
        (profile.flags & DARTPLANT_PROFILE_RAW_GP_ARGUMENTS) == 0 ||
        (profile.flags & DARTPLANT_PROFILE_RAW_GP_RESULT) == 0 || profile.argument_count > 8 ||
        !IsSupportedLocation(profile.result_location)) {
        SetLastError("callback hook requires a validated ARM64 GP profile");
        return false;
    }
    if (((profile.flags & DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS) != 0 &&
         (profile.flags & DARTPLANT_PROFILE_RAW_GP_ARGUMENTS) == 0) ||
        ((profile.flags & DARTPLANT_PROFILE_TAGGED_GP_RESULT) != 0 &&
         (profile.flags & DARTPLANT_PROFILE_RAW_GP_RESULT) == 0)) {
        SetLastError("tagged GP profile flags require raw GP location mappings");
        return false;
    }
    for (uint32_t index = 0; index < profile.argument_count; ++index) {
        if (!IsSupportedLocation(profile.argument_locations[index])) {
            SetLastError("callback hook argument register is invalid");
            return false;
        }
    }
    return true;
}

DartPlantStatus FinishUnhookLocked(DartPlantHook* hook) {
    const uintptr_t target = HookTarget(hook);
    if (target == 0 || State().host.unhook == nullptr ||
        State().host.unhook(reinterpret_cast<void*>(target)) != 0) {
        std::lock_guard hook_lock(hook->mutex);
        hook->state = HookRecordState::kFailed;
        SetLastError("host unhook function failed");
        return DARTPLANT_UNHOOK_FAILED;
    }
    if (hook->code_target != nullptr) hook->code_target->UnbindHookRecord(hook);
    std::lock_guard hook_lock(hook->mutex);
    hook->state = HookRecordState::kUnhooked;
    return DARTPLANT_OK;
}

DartPlantStatus UnhookRecordLocked(DartPlantHook* hook) {
    {
        std::lock_guard hook_lock(hook->mutex);
        if (hook->state == HookRecordState::kUnhooked) return DARTPLANT_OK;
        if (hook->state == HookRecordState::kRetired) {
            SetLastError("hook target mapping is retired and cannot be safely unhooked");
            return DARTPLANT_UNHOOK_FAILED;
        }
        if (hook->state == HookRecordState::kUnhooking) {
            return DARTPLANT_OK;
        }
        hook->state = HookRecordState::kUnhooking;
        hook->active.store(false, std::memory_order_release);
        for (const auto& listener : hook->listeners) {
            listener->active.store(false, std::memory_order_release);
        }
        hook->listeners.clear();
        if (hook->in_flight != 0) return DARTPLANT_OK;
    }
    return FinishUnhookLocked(hook);
}

bool CanDestroyLocked(const DartPlantHook* hook) {
    return hook->state == HookRecordState::kUnhooked && hook->in_flight == 0 &&
           hook->listener_handles == 0;
}

}  // namespace

RuntimeState& State() {
    static RuntimeState state;
    return state;
}

void InstallHostApi(const DartPlantNativeApiEntries* entries) {
    std::lock_guard lock(State().mutex);
    if (entries == nullptr || entries->version < 2 || entries->hook_func == nullptr ||
        entries->unhook_func == nullptr) {
        State().host = {};
        SetLastError("host API version or function pointers are invalid");
        return;
    }
    State().host = {entries->hook_func, entries->unhook_func};
    ClearLastError();
}

void RefreshModules() {
    std::lock_guard lock(State().mutex);
    State().modules = EnumerateModules();
}

DartPlantStatus InvalidateRuntimeHooks(
    const std::shared_ptr<std::atomic_uint64_t>& runtime_generation) {
    if (runtime_generation == nullptr) return DARTPLANT_OK;
    std::lock_guard lock(State().mutex);
    DartPlantStatus status = DARTPLANT_OK;
    for (const auto& hook : Hooks()) {
        if (hook->runtime_generation != runtime_generation) continue;
        {
            std::lock_guard hook_lock(hook->mutex);
            if (hook->state == HookRecordState::kRetired) continue;
        }
        const DartPlantStatus unhook_status = UnhookRecordLocked(hook.get());
        if (unhook_status != DARTPLANT_OK) status = unhook_status;
    }
    return status;
}

void RetireRuntimeHooks(const std::shared_ptr<std::atomic_uint64_t>& runtime_generation) {
    if (runtime_generation == nullptr) return;
    std::lock_guard lock(State().mutex);
    for (const auto& hook : Hooks()) {
        if (hook->runtime_generation != runtime_generation) continue;
        std::lock_guard hook_lock(hook->mutex);
        if (hook->state == HookRecordState::kUnhooked) continue;
        hook->active.store(false, std::memory_order_release);
        for (const auto& listener : hook->listeners) {
            listener->active.store(false, std::memory_order_release);
        }
        hook->listeners.clear();
        hook->state = HookRecordState::kRetired;
    }
}

bool IsTargetHooked(uintptr_t target) {
    DartPlantHook* hook = FindHookLocked(target);
    return hook != nullptr && hook->active.load(std::memory_order_acquire);
}

DartPlantStatus InstallHook(const std::shared_ptr<DartCodeTarget>& code_target, void* replacement,
                            void** backup, DartPlantHook** out_hook) {
    const uintptr_t target = code_target == nullptr ? 0 : code_target->entry;
    if (target == 0 || replacement == nullptr || backup == nullptr || out_hook == nullptr) {
        SetLastError("hook arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(State().mutex);
    if (State().host.hook == nullptr || State().host.unhook == nullptr) {
        SetLastError("host hook API is not initialized");
        return DARTPLANT_HOST_API_UNAVAILABLE;
    }
    if (FindHookLocked(target) != nullptr) {
        SetLastError("target is already hooked");
        return DARTPLANT_ALREADY_HOOKED;
    }
    void* original = nullptr;
    if (State().host.hook(reinterpret_cast<void*>(target), replacement, &original) != 0 ||
        original == nullptr) {
        SetLastError("host hook function failed");
        return DARTPLANT_HOOK_FAILED;
    }
    auto hook = std::make_unique<DartPlantHook>();
    hook->code_target = code_target;
    hook->backup = original;
    hook->active.store(true, std::memory_order_release);
    hook->state = HookRecordState::kInstalled;
    hook->code_target->BindHookRecord(hook.get());
    *backup = original;
    *out_hook = hook.get();
    Hooks().push_back(std::move(hook));
    return DARTPLANT_OK;
}

DartPlantStatus InstallHook(uintptr_t target, void* replacement, void** backup,
                            DartPlantHook** out_hook) {
    if (target == 0) {
        SetLastError("hook target is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto code_target = std::make_shared<DartCodeTarget>();
    code_target->id = target;
    code_target->entry = target;
    return InstallHook(code_target, replacement, backup, out_hook);
}

DartPlantStatus RemoveHook(DartPlantHook* hook) {
    if (hook == nullptr) {
        SetLastError("hook is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(State().mutex);
    return UnhookRecordLocked(hook);
}

void ResetHooks() {
    std::lock_guard lock(State().mutex);
    for (auto& hook : Hooks()) {
        if (hook->state != HookRecordState::kUnhooked) {
            UnhookRecordLocked(hook.get());
        }
    }
    Hooks().erase(std::remove_if(Hooks().begin(), Hooks().end(),
                                 [](const auto& hook) {
                                     std::lock_guard hook_lock(hook->mutex);
                                     if (!CanDestroyLocked(hook.get())) return false;
                                     DestroyArm64CallbackStub(hook->replacement_entry,
                                                              hook->replacement_entry_size);
                                     VmAdapterReleaseHook(hook->vm_adapter);
                                     return true;
                                 }),
                  Hooks().end());
}

void ReleaseHook(DartPlantHook* hook) {
    if (hook == nullptr) return;
    std::lock_guard lock(State().mutex);
    std::lock_guard hook_lock(hook->mutex);
    hook->release_requested = true;
    // Callback records and executable stubs are reclaimed by ResetHooks once
    // there are no in-flight invocations. This avoids executable-code UAF when
    // the host API cannot provide a global trampoline quiescence barrier.
}

bool BeginInvocation(DartPlantHook* hook,
                     std::vector<std::shared_ptr<DartPlantListenerRecord>>* listeners) {
    if (hook == nullptr || listeners == nullptr) return false;
    std::lock_guard lock(hook->mutex);
    if (!hook->has_method || hook->state == HookRecordState::kFailed ||
        hook->state == HookRecordState::kRetired) {
        return false;
    }
    if (hook->runtime_generation != nullptr &&
        hook->runtime_generation->load(std::memory_order_acquire) !=
            hook->expected_runtime_generation) {
        SetLastError("callback hook belongs to a stale runtime generation");
        return false;
    }
    ++hook->in_flight;
    listeners->clear();
    if (hook->active.load(std::memory_order_acquire)) {
        *listeners = hook->listeners;
        for (const auto& listener : *listeners) {
            listener->in_flight.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    return true;
}

void InvocationExited(DartPlantHook* hook) {
    if (hook == nullptr) return;
    bool finish_unhook = false;
    {
        std::lock_guard lock(hook->mutex);
        if (hook->in_flight != 0) --hook->in_flight;
        finish_unhook = hook->in_flight == 0 && hook->state == HookRecordState::kUnhooking;
    }
    if (finish_unhook) {
        std::lock_guard state_lock(State().mutex);
        bool still_unhooking = false;
        {
            std::lock_guard hook_lock(hook->mutex);
            still_unhooking = hook->state == HookRecordState::kUnhooking;
        }
        if (still_unhooking) FinishUnhookLocked(hook);
    }
}

DartPlantStatus InstallCallbackHook(const DartPlantMethod* method,
                                    const DartPlantRuntimeProfile& profile,
                                    const DartPlantHookOptions& options, int32_t priority,
                                    DartPlantHook** out_hook, DartPlantListener** out_listener,
                                    uint64_t validated_null_value,
                                    std::shared_ptr<std::atomic_uint64_t> runtime_generation,
                                    uint64_t expected_runtime_generation) {
    const uintptr_t target = MethodTarget(method);
    if (target == 0 || method == nullptr || method->function == nullptr ||
        method->function->code_target == nullptr ||
        (out_hook == nullptr && out_listener == nullptr) || !ValidCallbackOptions(options)) {
        SetLastError("callback hook arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (!ValidateProfile(profile)) return DARTPLANT_UNSUPPORTED_ABI;
    if (method->function->code_target->IsShared() && !AllowsSharedCode(options)) {
        SetLastError(
            "method callback target is shared by multiple Dart Functions; explicit shared-code opt-in is required");
        return DARTPLANT_SHARED_CODE_ENTRY;
    }

    std::lock_guard lock(State().mutex);
    if (State().host.hook == nullptr || State().host.unhook == nullptr) {
        SetLastError("host hook API is not initialized");
        return DARTPLANT_HOST_API_UNAVAILABLE;
    }
    if (FindHookLocked(target) != nullptr) {
        SetLastError("target is already hooked");
        return DARTPLANT_ALREADY_HOOKED;
    }

    auto hook = std::make_unique<DartPlantHook>();
    hook->code_target = method->function->code_target;
    hook->has_method = true;
    hook->shared_code_opt_in = AllowsSharedCode(options);
    hook->method_storage = std::make_unique<DartPlantMethod>(*method);
    hook->profile = profile;
    hook->options = options;
    hook->vm_adapter = options.vm_adapter;
    hook->validated_null_value = validated_null_value;
    hook->runtime_generation = std::move(runtime_generation);
    hook->expected_runtime_generation = expected_runtime_generation;
    hook->state = HookRecordState::kInstalling;
    auto first_listener = MakeListenerLocked(hook.get(), method, options, priority);
    InsertListenerLocked(hook.get(), first_listener);
    hook->replacement_entry = CreateArm64CallbackStub(hook.get(), &hook->replacement_entry_size);
    if (hook->replacement_entry == nullptr) {
        SetLastError("failed to allocate ARM64 callback stub");
        return DARTPLANT_HOOK_FAILED;
    }

    void* original = nullptr;
    if (State().host.hook(reinterpret_cast<void*>(target), hook->replacement_entry, &original) !=
            0 ||
        original == nullptr) {
        DestroyArm64CallbackStub(hook->replacement_entry, hook->replacement_entry_size);
        SetLastError("host hook function failed");
        return DARTPLANT_HOOK_FAILED;
    }
    hook->backup = original;
    hook->active.store(true, std::memory_order_release);
    hook->state = HookRecordState::kInstalled;
    hook->code_target->BindHookRecord(hook.get());
    if (out_hook != nullptr) *out_hook = hook.get();
    if (out_listener != nullptr) {
        auto* listener = new DartPlantListener;
        listener->hook = hook.get();
        listener->record = std::move(first_listener);
        ++hook->listener_handles;
        *out_listener = listener;
    }
    VmAdapterRetainHook(hook->vm_adapter);
    Hooks().push_back(std::move(hook));
    return DARTPLANT_OK;
}

DartPlantStatus AddCallbackListener(DartPlantHook* hook, const DartPlantMethod* requested_method,
                                    const DartPlantHookOptions& options, int32_t priority,
                                    DartPlantListener** out_listener,
                                    const std::shared_ptr<std::atomic_uint64_t>& runtime_generation,
                                    uint64_t expected_runtime_generation) {
    if (hook == nullptr || requested_method == nullptr || out_listener == nullptr ||
        !ValidCallbackOptions(options)) {
        SetLastError("listener arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const uintptr_t requested_target = MethodTarget(requested_method);
    if (requested_target != 0 && requested_target != HookTarget(hook)) {
        SetLastError("listener method does not resolve to the active CodeTarget");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (requested_method->function != nullptr &&
        requested_method->function->code_target != nullptr &&
        requested_method->function->code_target->IsShared() &&
        (!hook->shared_code_opt_in || !AllowsSharedCode(options))) {
        SetLastError(
            "method listener target is shared by multiple Dart Functions; both the physical hook and listener require explicit shared-code opt-in");
        return DARTPLANT_SHARED_CODE_ENTRY;
    }
    std::lock_guard lock(hook->mutex);
    if (hook->runtime_generation != runtime_generation ||
        (runtime_generation != nullptr &&
         (hook->expected_runtime_generation != expected_runtime_generation ||
          runtime_generation->load(std::memory_order_acquire) != expected_runtime_generation))) {
        SetLastError("listener runtime generation does not match the active hook");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (hook->state != HookRecordState::kInstalled ||
        !hook->active.load(std::memory_order_acquire)) {
        SetLastError("hook is not active");
        return DARTPLANT_UNHOOK_FAILED;
    }
    if (options.vm_adapter != hook->vm_adapter) {
        SetLastError("listener VM adapter does not match the hook adapter");
        return DARTPLANT_VM_ISOLATE_MISMATCH;
    }
    auto record = MakeListenerLocked(hook, requested_method, options, priority);
    InsertListenerLocked(hook, record);
    auto* listener = new DartPlantListener;
    listener->hook = hook;
    listener->record = std::move(record);
    ++hook->listener_handles;
    *out_listener = listener;
    return DARTPLANT_OK;
}

DartPlantStatus AddCallbackListenerForMethod(
    const DartPlantMethod* method, const DartPlantHookOptions& options, int32_t priority,
    DartPlantListener** out_listener,
    const std::shared_ptr<std::atomic_uint64_t>& runtime_generation,
    uint64_t expected_runtime_generation) {
    if (method == nullptr) {
        SetLastError("listener method is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const uintptr_t target = MethodTarget(method);
    std::lock_guard lock(State().mutex);
    DartPlantHook* hook = FindHookLocked(target);
    if (hook == nullptr || !hook->has_method) {
        SetLastError("method does not have an active callback hook");
        return DARTPLANT_NOT_INITIALIZED;
    }
    return AddCallbackListener(hook, method, options, priority, out_listener, runtime_generation,
                               expected_runtime_generation);
}

}  // namespace dartplant
