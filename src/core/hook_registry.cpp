// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/internal.h"

namespace dartplant {
namespace {

constexpr uint32_t kSupportedCallbackProfileFlags =
    DARTPLANT_PROFILE_RAW_GP_ARGUMENTS | DARTPLANT_PROFILE_RAW_GP_RESULT |
    DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS | DARTPLANT_PROFILE_TAGGED_GP_RESULT;

std::vector<std::unique_ptr<DartPlantHook>>& Hooks() {
    static std::vector<std::unique_ptr<DartPlantHook>> hooks;
    return hooks;
}

// A published entry veneer can already be in an instruction-fetch window when
// the host reports the physical hook as removed. Keep the veneer and the hook
// object it names alive forever; restoring the entry bytes removes the normal
// reachability, while this list covers stale CPU fetches without a quiescence
// barrier from the host backend.
std::vector<std::unique_ptr<DartPlantHook>>& PublishedCallbackHooks() {
    static std::vector<std::unique_ptr<DartPlantHook>> hooks;
    return hooks;
}

bool IsSupportedArgumentRegister(uint8_t reg) {
    // x16/x17 are veneer scratch registers. x30 is the architectural Dart
    // caller LR and remains untouched by real-Dart entry hooks so exception
    // stack walking continues to see the original caller PC.
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

bool SnapshotEntryTarget(const std::shared_ptr<DartEntryTarget>& code_target,
                         std::vector<uint8_t>* out_bytes) {
    if (out_bytes == nullptr) return false;
    out_bytes->clear();
    if (code_target == nullptr || code_target->entry == 0 || code_target->code_size == 0)
        return true;
    try {
        out_bytes->resize(code_target->code_size);
    } catch (...) {
        return false;
    }
    std::memcpy(out_bytes->data(), reinterpret_cast<const void*>(code_target->entry),
                code_target->code_size);
    return true;
}

bool PrepareManagedPatchSnapshot(DartPlantHook* hook, const std::vector<uint8_t>& pristine) {
    if (hook == nullptr || hook->code_target == nullptr ||
        pristine.size() != hook->code_target->code_size) {
        return false;
    }
    hook->managed_backend_patches.clear();
    if (pristine.empty()) return true;
    try {
        ManagedCodePatch patch;
        patch.address = hook->code_target->entry;
        patch.original_bytes = pristine;
        patch.patched_bytes.resize(pristine.size());
        hook->managed_backend_patches.push_back(std::move(patch));
    } catch (...) {
        hook->managed_backend_patches.clear();
        return false;
    }
    return true;
}

bool CaptureManagedPatchedBytes(DartPlantHook* hook) {
    if (hook == nullptr || hook->code_target == nullptr) return false;
    if (hook->code_target->code_size == 0) return hook->managed_backend_patches.empty();
    if (hook->managed_backend_patches.size() != 1) return false;
    ManagedCodePatch& patch = hook->managed_backend_patches[0];
    if (patch.address != hook->code_target->entry ||
        patch.original_bytes.size() != hook->code_target->code_size ||
        patch.patched_bytes.size() != hook->code_target->code_size) {
        return false;
    }
    std::memcpy(patch.patched_bytes.data(), reinterpret_cast<const void*>(patch.address),
                patch.patched_bytes.size());
    return true;
}

void OverlayOriginalBytes(uintptr_t start, uintptr_t end, const ManagedCodePatch& patch,
                          std::vector<uint8_t>* bytes) {
    if (bytes == nullptr || patch.address >= end || patch.original_bytes.empty() ||
        patch.original_bytes.size() != patch.patched_bytes.size()) {
        return;
    }
    const uintptr_t patch_end = patch.address > UINTPTR_MAX - patch.original_bytes.size()
                                    ? UINTPTR_MAX
                                    : patch.address + patch.original_bytes.size();
    if (patch_end <= start) return;
    const uintptr_t overlap_start = std::max(start, patch.address);
    const uintptr_t overlap_end = std::min(end, patch_end);
    if (overlap_start >= overlap_end) return;
    const size_t destination = static_cast<size_t>(overlap_start - start);
    const size_t source = static_cast<size_t>(overlap_start - patch.address);
    const size_t count = static_cast<size_t>(overlap_end - overlap_start);
    for (size_t index = 0; index < count; ++index) {
        const size_t dst = destination + index;
        const size_t src = source + index;
        if ((*bytes)[dst] == patch.patched_bytes[src]) {
            (*bytes)[dst] = patch.original_bytes[src];
        }
    }
}

bool NormalizeManagedCodeBytesLocked(uintptr_t start, size_t size, std::vector<uint8_t>* bytes) {
    if (bytes == nullptr || start == 0 || size == 0 || start > UINTPTR_MAX - size) return false;
    const uintptr_t end = start + size;
    try {
        bytes->resize(size);
    } catch (...) {
        return false;
    }
    std::memcpy(bytes->data(), reinterpret_cast<const void*>(start), size);

    // Backend patches are entry-local and compose in installation order. Peel
    // all of them first in reverse order. Payload-level RET patches are older
    // than the backend patch for each callback entry and are peeled once after
    // all entry patches, restoring the shared Code body.
    for (auto hook_it = Hooks().rbegin(); hook_it != Hooks().rend(); ++hook_it) {
        const auto& owned_hook = *hook_it;
        std::lock_guard hook_lock(owned_hook->mutex);
        if (owned_hook->state == HookRecordState::kUnhooked) continue;
        for (const ManagedCodePatch& patch : owned_hook->managed_backend_patches) {
            OverlayOriginalBytes(start, end, patch, bytes);
        }
    }

    std::vector<DartCodePayload*> seen_payloads;
    for (auto hook_it = Hooks().rbegin(); hook_it != Hooks().rend(); ++hook_it) {
        const auto& owned_hook = *hook_it;
        std::shared_ptr<DartCodePayload> payload;
        {
            std::lock_guard hook_lock(owned_hook->mutex);
            if (owned_hook->state == HookRecordState::kUnhooked ||
                owned_hook->code_target == nullptr) {
                continue;
            }
            payload = owned_hook->code_target->payload;
        }
        if (payload == nullptr || std::find(seen_payloads.begin(), seen_payloads.end(),
                                            payload.get()) != seen_payloads.end()) {
            continue;
        }
        seen_payloads.push_back(payload.get());
        std::lock_guard payload_lock(payload->mutex);
        for (const Arm64ReturnPatch& patch : payload->return_patches) {
            const ManagedCodePatch return_patch = {
                .address = patch.address,
                .original_bytes =
                    {
                        static_cast<uint8_t>(patch.original_instruction & 0xffU),
                        static_cast<uint8_t>((patch.original_instruction >> 8) & 0xffU),
                        static_cast<uint8_t>((patch.original_instruction >> 16) & 0xffU),
                        static_cast<uint8_t>((patch.original_instruction >> 24) & 0xffU),
                    },
                .patched_bytes =
                    {
                        static_cast<uint8_t>(patch.patched_instruction & 0xffU),
                        static_cast<uint8_t>((patch.patched_instruction >> 8) & 0xffU),
                        static_cast<uint8_t>((patch.patched_instruction >> 16) & 0xffU),
                        static_cast<uint8_t>((patch.patched_instruction >> 24) & 0xffU),
                    },
            };
            OverlayOriginalBytes(start, end, return_patch, bytes);
        }
    }
    return true;
}

bool PreparePayloadPristineLocked(const std::shared_ptr<DartCodePayload>& payload) {
    if (payload == nullptr || payload->start == 0 || payload->instructions_length == 0)
        return false;
    {
        std::lock_guard payload_lock(payload->mutex);
        if (!payload->pristine_bytes.empty()) {
            return payload->pristine_bytes.size() == payload->instructions_length;
        }
    }
    std::vector<uint8_t> normalized;
    if (!NormalizeManagedCodeBytesLocked(payload->start, payload->instructions_length,
                                         &normalized)) {
        return false;
    }
    std::lock_guard payload_lock(payload->mutex);
    if (payload->pristine_bytes.empty()) payload->pristine_bytes = std::move(normalized);
    return payload->pristine_bytes.size() == payload->instructions_length;
}

DartPlantHook* FindHookLocked(uintptr_t target) {
    const auto found = std::find_if(Hooks().begin(), Hooks().end(), [target](const auto& hook) {
        std::lock_guard hook_lock(hook->mutex);
        return HookTarget(hook.get()) == target && hook->state != HookRecordState::kUnhooked;
    });
    return found == Hooks().end() ? nullptr : found->get();
}

bool ValidateProfile(const DartPlantRuntimeProfile& profile) {
    if ((profile.flags & ~kSupportedCallbackProfileFlags) != 0 ||
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

bool HasLegacyCallbackMapping(const DartPlantRuntimeProfile& profile) {
    return (profile.flags & kSupportedCallbackProfileFlags) != 0;
}

DartPlantStatus FinishUnhookLocked(DartPlantHook* hook) {
    const uintptr_t target = HookTarget(hook);
    // Synthetic internal tests can construct a hook without the public
    // installation path. Real installed hooks always retain their own binding.
    const auto* host_binding = hook->host_binding == nullptr
                                   ? State().host.binding.load(std::memory_order_acquire)
                                   : hook->host_binding;
    const DartPlantHostUnhookCallback host_unhook =
        host_binding == nullptr ? nullptr : host_binding->unhook;
    if (hook->backend_installed) {
        if (target == 0 || host_unhook == nullptr ||
            host_unhook(host_binding->user_data, reinterpret_cast<void*>(target)) != 0) {
            std::lock_guard hook_lock(hook->mutex);
            hook->state = HookRecordState::kFailed;
            SetLastError("host unhook function failed");
            return DARTPLANT_UNHOOK_FAILED;
        }
        hook->backend_installed = false;
    }
    // Make stale callback veneers choose passthrough before return/exception
    // ownership is torn down. entry_published stays set until the hook is
    // moved to the retired list, so an unexpected fetch cannot enter a
    // partially-reset callback record.
    hook->active.store(false, std::memory_order_release);
    if (!RestoreArm64ReturnInterception(hook)) {
        std::lock_guard hook_lock(hook->mutex);
        hook->state = HookRecordState::kFailed;
        SetLastError("failed to restore Dart return interception");
        return DARTPLANT_UNHOOK_FAILED;
    }
    hook->entry_published.store(false, std::memory_order_release);
    hook->entry_ready.store(false, std::memory_order_release);
    ReleaseArm64ExceptionBridgeConsumer(hook);
    if (hook->code_target != nullptr) hook->code_target->UnbindHookRecord(hook);
    DartPlantVmAdapter* adapter_to_release = nullptr;
    {
        std::lock_guard hook_lock(hook->mutex);
        hook->state = HookRecordState::kUnhooked;
        if (hook->vm_adapter_retained) {
            adapter_to_release = hook->vm_adapter;
            hook->vm_adapter = nullptr;
            hook->vm_adapter_retained = false;
        }
    }
    VmAdapterReleaseHook(adapter_to_release);
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

void InstallHostApi(const DartPlantHostApi* api) {
    if (api == nullptr || api->struct_size < DARTPLANT_HOST_API_LEGACY_SIZE ||
        api->version < DARTPLANT_HOST_API_VERSION || api->hook == nullptr ||
        api->unhook == nullptr) {
        State().host.binding.store(nullptr, std::memory_order_release);
        SetLastError("host API version or function pointers are invalid");
        return;
    }
    auto* binding = new HostApiBinding();
    binding->user_data = api->user_data;
    binding->hook = api->hook;
    binding->unhook = api->unhook;
    binding->hook_with_publication =
        api->struct_size >= sizeof(DartPlantHostApi) ? api->hook_with_publication : nullptr;
    // Bindings are intentionally leaked while any hook may still own the
    // callback ABI. Replacing the default must not invalidate a stale
    // host/unhook function pointer used by an existing hook.
    (void) State().host.binding.exchange(binding, std::memory_order_acq_rel);
    ClearLastError();
}

void ClearHostApi(const HostApiBinding* expected_binding) {
    if (expected_binding == nullptr) return;
    const HostApiBinding* expected = expected_binding;
    (void) State().host.binding.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
}

void RefreshModules() { ReplaceModules(EnumerateModules()); }

void ReplaceModules(std::vector<ModuleImage> modules) {
    std::lock_guard lock(State().mutex);
    State().modules = std::move(modules);
}

std::string FingerprintCodeWithManagedPatches(const void* address, size_t size) {
    if (address == nullptr || size == 0) return FingerprintCode(address, size);
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    if (start > UINTPTR_MAX - size) return {};
    // Hook installation/unhook and the managed-patch registry are serialized by
    // State().mutex. Copy the live bytes under the same lock so the snapshot and
    // patch overlay describe one coherent DartPlant hook lifecycle state.
    std::lock_guard state_lock(State().mutex);
    std::vector<uint8_t> normalized;
    if (!NormalizeManagedCodeBytesLocked(start, size, &normalized)) return {};
    return FingerprintCode(normalized.data(), normalized.size());
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
        ReleaseArm64ExceptionBridgeConsumer(hook.get());
    }
}

bool IsTargetHooked(uintptr_t target) {
    DartPlantHook* hook = FindHookLocked(target);
    return hook != nullptr && hook->active.load(std::memory_order_acquire);
}

DartPlantStatus InstallHook(const std::shared_ptr<DartEntryTarget>& code_target, void* replacement,
                            void** backup, DartPlantHook** out_hook) {
    const uintptr_t target = code_target == nullptr ? 0 : code_target->entry;
    if (target == 0 || replacement == nullptr || backup == nullptr || out_hook == nullptr) {
        SetLastError("hook arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(State().mutex);
    const auto* host_binding = State().host.binding.load(std::memory_order_acquire);
    if (host_binding == nullptr || host_binding->hook == nullptr ||
        host_binding->unhook == nullptr) {
        SetLastError("host hook API is not initialized");
        return DARTPLANT_HOST_API_UNAVAILABLE;
    }
    if (FindHookLocked(target) != nullptr) {
        SetLastError("target is already hooked");
        return DARTPLANT_ALREADY_HOOKED;
    }
    try {
        if (Hooks().size() == Hooks().max_size()) throw std::bad_alloc();
        Hooks().reserve(Hooks().size() + 1);
    } catch (...) {
        SetLastError("failed to reserve hook registry ownership");
        return DARTPLANT_HOOK_FAILED;
    }
    std::vector<uint8_t> pristine;
    if (!SnapshotEntryTarget(code_target, &pristine)) {
        SetLastError("failed to snapshot hook target before installation");
        return DARTPLANT_HOOK_FAILED;
    }
    auto hook = std::make_unique<DartPlantHook>();
    hook->code_target = code_target;
    if (!PrepareManagedPatchSnapshot(hook.get(), pristine)) {
        SetLastError("failed to prepare hook patch integrity snapshot");
        return DARTPLANT_HOOK_FAILED;
    }
    void* original = nullptr;
    if (host_binding->hook(host_binding->user_data, reinterpret_cast<void*>(target), replacement,
                           &original) != 0 ||
        original == nullptr) {
        SetLastError("host hook function failed");
        return DARTPLANT_HOOK_FAILED;
    }
    hook->backend_installed = true;
    if (!CaptureManagedPatchedBytes(hook.get())) {
        // All buffers were allocated before calling the backend, so reaching
        // this branch means an internal invariant failed rather than OOM. Keep
        // ownership only if the backend cannot restore its own patch.
        if (host_binding->unhook(host_binding->user_data, reinterpret_cast<void*>(target)) != 0) {
            hook->backup.store(original, std::memory_order_release);
            hook->host_binding = host_binding;
            hook->active.store(false, std::memory_order_release);
            hook->state = HookRecordState::kFailed;
            Hooks().push_back(std::move(hook));
        } else {
            hook->backend_installed = false;
        }
        SetLastError("failed to record hook backend code patches");
        return DARTPLANT_HOOK_FAILED;
    }
    hook->backup.store(original, std::memory_order_release);
    hook->host_binding = host_binding;
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
    auto code_target = std::make_shared<DartEntryTarget>();
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
    try {
        if (Hooks().size() >
            PublishedCallbackHooks().max_size() - PublishedCallbackHooks().size()) {
            SetLastError("too many published callback hooks to retain");
            return;
        }
        PublishedCallbackHooks().reserve(PublishedCallbackHooks().size() + Hooks().size());
    } catch (...) {
        SetLastError("failed to reserve published callback hook ownership");
        return;
    }
    bool incomplete = false;
    for (auto& hook : Hooks()) {
        if (hook->state != HookRecordState::kUnhooked) {
            if (UnhookRecordLocked(hook.get()) != DARTPLANT_OK) incomplete = true;
        }
    }
    auto& published = PublishedCallbackHooks();
    for (auto it = Hooks().begin(); it != Hooks().end();) {
        DartPlantVmAdapter* adapter_to_release = nullptr;
        std::unique_ptr<DartPlantHook> published_hook;
        {
            auto& hook = *it;
            std::lock_guard hook_lock(hook->mutex);
            if (!CanDestroyLocked(hook.get())) {
                ++it;
                continue;
            }
            if (hook->replacement_entry != nullptr) {
                if (hook->vm_adapter_retained) adapter_to_release = hook->vm_adapter;
                hook->vm_adapter = nullptr;
                hook->vm_adapter_retained = false;
                published_hook = std::move(hook);
            } else {
                DestroyArm64CallbackStub(hook->replacement_entry, hook->replacement_entry_size);
                if (hook->vm_adapter_retained) adapter_to_release = hook->vm_adapter;
                hook->vm_adapter_retained = false;
            }
        }
        it = Hooks().erase(it);
        if (published_hook != nullptr) published.push_back(std::move(published_hook));
        if (adapter_to_release != nullptr) VmAdapterReleaseHook(adapter_to_release);
    }
    if (incomplete) SetLastError("one or more hooks could not be reset");
}

void ReleaseHook(DartPlantHook* hook) {
    if (hook == nullptr) return;
    std::lock_guard lock(State().mutex);
    std::lock_guard hook_lock(hook->mutex);
    hook->release_requested = true;
    // Callback records are reclaimed by ResetHooks once there are no in-flight
    // invocations. Published entry stubs and the hook objects they name are
    // retained by PublishedCallbackHooks for stale instruction fetches.
}

bool BeginInvocation(DartPlantHook* hook,
                     std::vector<std::shared_ptr<DartPlantListenerRecord>>* listeners) {
    if (hook == nullptr || listeners == nullptr) return false;
    std::lock_guard lock(hook->mutex);
    if (!hook->has_method || hook->state == HookRecordState::kFailed ||
        hook->state == HookRecordState::kRetired || hook->state == HookRecordState::kUnhooking ||
        hook->state == HookRecordState::kUnhooked) {
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

DartPlantStatus InstallCallbackHook(
    const DartPlantMethod* method, const DartPlantRuntimeProfile& profile,
    const DartPlantHookOptions& options, int32_t priority, DartPlantHook** out_hook,
    DartPlantListener** out_listener, uint64_t validated_null_value,
    std::shared_ptr<std::atomic_uint64_t> runtime_generation, uint64_t expected_runtime_generation,
    uint64_t validated_bool_true_value, uint64_t validated_bool_false_value,
    std::shared_ptr<const abi::DartCallLayout> call_layout) {
    const uintptr_t target = MethodTarget(method);
    if (target == 0 || method == nullptr || method->function == nullptr ||
        method->function->code_target == nullptr ||
        (out_hook == nullptr && out_listener == nullptr) || !ValidCallbackOptions(options)) {
        SetLastError("callback hook arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    // A verified per-Function DartCallLayout supersedes the legacy/manual
    // argument_locations[] escape hatch. Keep both paths independent so the
    // normal runtime API can use an otherwise conservative RuntimeProfile.
    // The trampoline itself is a raw instrumentation primitive and does not
    // require typed argument knowledge. Validate the legacy mapping only when
    // the caller explicitly opts into it; otherwise raw register/context APIs
    // remain available while typed argument/result APIs fail closed.
    if (call_layout == nullptr && HasLegacyCallbackMapping(profile) && !ValidateProfile(profile)) {
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if ((profile.flags & ~kSupportedCallbackProfileFlags) != 0) {
        SetLastError("callback profile contains unsupported flags");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (call_layout != nullptr && call_layout->dart_sp_register >= 31) {
        SetLastError("verified DartCallLayout has an invalid Dart SP register");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (options.vm_adapter != nullptr &&
        method->function->source != DartFunctionSource::kSynthetic) {
        // A real Dart callback may expose the VM adapter only when every tagged
        // live location is known and the host provides both the generated-root
        // lease and the actual Thread generated<->native safepoint transition.
        // Dart_EnterScope alone is not a transition and must never be treated
        // as one.
        if (call_layout == nullptr ||
            !VmAdapterSupportsGeneratedCallbackBridge(options.vm_adapter)) {
            SetLastError(
                "VM-adapter callbacks on Dart code require a verified DartCallLayout and a GC-safe generated/native bridge");
            return DARTPLANT_UNSUPPORTED_ABI;
        }
    }
    if (method->function->source != DartFunctionSource::kSynthetic &&
        method->function->thread_jump_to_frame_entry_point_offset == 0) {
        SetLastError("Dart callback hook requires an exact exception-unwind Thread profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (method->function->code_target->IsShared() && !AllowsSharedCode(options)) {
        SetLastError(
            "method callback target is shared by multiple Dart Functions; explicit shared-code opt-in is required");
        return DARTPLANT_SHARED_CODE_ENTRY;
    }
    if (method->function->code_target->IsShared() && call_layout != nullptr) {
        // A physical shared Code entry cannot prove which logical Function
        // reached it. Shared-code callbacks remain available through the raw
        // profile path, but typed interpretation is deliberately suppressed.
        call_layout.reset();
    }

    std::lock_guard lock(State().mutex);
    const auto* host_binding = State().host.binding.load(std::memory_order_acquire);
    if (host_binding == nullptr || host_binding->hook == nullptr ||
        host_binding->unhook == nullptr) {
        SetLastError("host hook API is not initialized");
        return DARTPLANT_HOST_API_UNAVAILABLE;
    }
    if (method->function->source != DartFunctionSource::kSynthetic &&
        host_binding->hook_with_publication == nullptr) {
        // A legacy hook(void**, backup) callback cannot publish backup into the
        // HookRecord before another mutator can fetch the replacement. Keep
        // raw/synthetic hooks available, but fail closed for real Dart code.
        SetLastError("real Dart callback hooks require a strict host publication adapter");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (FindHookLocked(target) != nullptr) {
        SetLastError("target is already hooked");
        return DARTPLANT_ALREADY_HOOKED;
    }
    try {
        if (Hooks().size() == Hooks().max_size()) throw std::bad_alloc();
        Hooks().reserve(Hooks().size() + 1);
    } catch (...) {
        SetLastError("failed to reserve hook registry ownership");
        return DARTPLANT_HOOK_FAILED;
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
    hook->validated_bool_true_value = validated_bool_true_value;
    hook->validated_bool_false_value = validated_bool_false_value;
    hook->call_layout = std::move(call_layout);
    hook->host_binding = host_binding;
    hook->runtime_generation = std::move(runtime_generation);
    hook->expected_runtime_generation = expected_runtime_generation;
    hook->state = HookRecordState::kInstalling;
    hook->entry_published.store(false, std::memory_order_relaxed);
    hook->entry_ready.store(false, std::memory_order_relaxed);
    auto first_listener = MakeListenerLocked(hook.get(), method, options, priority);
    InsertListenerLocked(hook.get(), first_listener);
    std::vector<uint8_t> pristine;
    if (!SnapshotEntryTarget(hook->code_target, &pristine)) {
        SetLastError("failed to snapshot callback target before installation");
        return DARTPLANT_HOOK_FAILED;
    }
    if (!PrepareManagedPatchSnapshot(hook.get(), pristine)) {
        SetLastError("failed to prepare callback patch integrity snapshot");
        return DARTPLANT_HOOK_FAILED;
    }
    std::unique_ptr<DartPlantListener> listener_handle;
    if (out_listener != nullptr) {
        try {
            listener_handle = std::make_unique<DartPlantListener>();
        } catch (...) {
            SetLastError("failed to allocate callback listener handle");
            return DARTPLANT_HOOK_FAILED;
        }
        listener_handle->hook = hook.get();
        listener_handle->record = first_listener;
    }
    if (method->function->source != DartFunctionSource::kSynthetic &&
        !PreparePayloadPristineLocked(hook->code_target->payload)) {
        SetLastError("failed to capture pristine Dart Code payload before return interception");
        return DARTPLANT_HOOK_FAILED;
    }
    hook->replacement_entry =
        CreateArm64CallbackStub(hook.get(), target, &hook->replacement_entry_size);
    if (hook->replacement_entry == nullptr) {
        SetLastError("failed to allocate ARM64 callback stub");
        return DARTPLANT_HOOK_FAILED;
    }

    if (method->function->source != DartFunctionSource::kSynthetic) {
        const DartPlantStatus return_status = InstallArm64ReturnInterception(hook.get());
        if (return_status != DARTPLANT_OK) {
            DestroyArm64CallbackStub(hook->replacement_entry, hook->replacement_entry_size);
            hook->replacement_entry = nullptr;
            hook->replacement_entry_size = 0;
            if (hook->payload_return_consumer) {
                hook->active.store(false, std::memory_order_release);
                hook->state = HookRecordState::kFailed;
                hook->listeners.clear();
                Hooks().push_back(std::move(hook));
            }
            return return_status;
        }
    }

    // Reserve the process-lifetime owner before the backend can publish the
    // replacement. No allocation is permitted after host_binding->hook().
    try {
        PublishedCallbackHooks().reserve(PublishedCallbackHooks().size() + 1);
    } catch (...) {
        DestroyArm64CallbackStub(hook->replacement_entry, hook->replacement_entry_size);
        hook->replacement_entry = nullptr;
        hook->replacement_entry_size = 0;
        SetLastError("failed to reserve callback entry publication ownership");
        return DARTPLANT_HOOK_FAILED;
    }

    void* original = nullptr;
    const auto publish_backup = [](void* user_data, void* backup) {
        auto* hook = static_cast<DartPlantHook*>(user_data);
        if (hook == nullptr || backup == nullptr) return;
        hook->backup.store(backup, std::memory_order_release);
        hook->backend_installed.store(true, std::memory_order_release);
        hook->entry_ready.store(true, std::memory_order_release);
    };
    DartPlantHostHookTransaction transaction{
        .struct_size = sizeof(DartPlantHostHookTransaction),
        .user_data = hook.get(),
        .backup_ready = publish_backup,
    };
    const int host_hook_status =
        host_binding->hook_with_publication != nullptr
            ? host_binding->hook_with_publication(host_binding->user_data,
                                                  reinterpret_cast<void*>(target),
                                                  hook->replacement_entry, &transaction)
            : host_binding->hook(host_binding->user_data, reinterpret_cast<void*>(target),
                                 hook->replacement_entry, &original);
    if (host_binding->hook_with_publication != nullptr) {
        original = hook->backup.load(std::memory_order_acquire);
    }
    if (host_hook_status != 0) {
        const bool published = host_hook_status == DARTPLANT_HOST_HOOK_FAILED_AFTER_PUBLISHED;
        hook->entry_published.store(published, std::memory_order_release);
        hook->entry_ready.store(published, std::memory_order_release);
        hook->active.store(false, std::memory_order_release);
        if (published) {
            // The adapter has already restored target, but a CPU may have
            // fetched the callback veneer. Restore only managed return sites;
            // keep both the veneer and HookRecord alive for stale fetches.
            hook->backend_installed.store(false, std::memory_order_release);
            (void) RestoreArm64ReturnInterception(hook.get());
            hook->state = HookRecordState::kFailedAfterPublished;
            hook->listeners.clear();
            Hooks().push_back(std::move(hook));
        } else if (RestoreArm64ReturnInterception(hook.get())) {
            DestroyArm64CallbackStub(hook->replacement_entry, hook->replacement_entry_size);
            hook->replacement_entry = nullptr;
            hook->replacement_entry_size = 0;
        } else {
            hook->state = HookRecordState::kFailed;
            hook->listeners.clear();
            Hooks().push_back(std::move(hook));
        }
        SetLastError("host hook function failed");
        return DARTPLANT_HOOK_FAILED;
    }
    if (original == nullptr) {
        // A successful host transaction without a callable backup violates the
        // HostApi contract. There is no safe original path to return to.
        SetLastError("host hook succeeded without an original trampoline");
        __builtin_trap();
    }
    hook->backend_installed.store(true, std::memory_order_release);
    hook->backup.store(original, std::memory_order_release);
    hook->entry_published.store(true, std::memory_order_release);
    hook->entry_ready.store(true, std::memory_order_release);
    if (!CaptureManagedPatchedBytes(hook.get())) {
        const bool backend_restored =
            host_binding->unhook(host_binding->user_data, reinterpret_cast<void*>(target)) == 0;
        hook->backend_installed.store(!backend_restored, std::memory_order_release);
        const bool returns_restored = RestoreArm64ReturnInterception(hook.get());
        if (backend_restored && returns_restored) {
            hook->active.store(false, std::memory_order_release);
            hook->state = HookRecordState::kFailed;
            hook->listeners.clear();
            Hooks().push_back(std::move(hook));
        } else {
            hook->host_binding = host_binding;
            hook->active.store(false, std::memory_order_release);
            hook->state = HookRecordState::kFailed;
            Hooks().push_back(std::move(hook));
        }
        SetLastError("failed to record callback backend code patches");
        return DARTPLANT_HOOK_FAILED;
    }
    if (method->function->source != DartFunctionSource::kSynthetic) {
        RegisterArm64ExceptionBridgeConsumer(hook.get());
    }
    hook->active.store(true, std::memory_order_release);
    hook->state = HookRecordState::kInstalled;
    hook->code_target->BindHookRecord(hook.get());
    if (out_hook != nullptr) *out_hook = hook.get();
    if (out_listener != nullptr) {
        ++hook->listener_handles;
        *out_listener = listener_handle.release();
    }
    VmAdapterRetainHook(hook->vm_adapter);
    hook->vm_adapter_retained = hook->vm_adapter != nullptr;
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
        SetLastError("listener method does not resolve to the active entry target");
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
