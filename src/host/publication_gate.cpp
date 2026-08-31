// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <thread>

#include "core/internal.h"

namespace dartplant {
namespace {

constexpr uint64_t kGateStateMask = UINT64_C(0xffffffff);
constexpr uint64_t kGateEntrantOne = UINT64_C(1) << 32;

uint64_t EncodeGateControl(HostPublicationGateState state, uint32_t entrants = 0) {
    return (static_cast<uint64_t>(entrants) << 32) | static_cast<uint32_t>(state);
}

HostPublicationGateState LoadGateState(const HostPublicationGate& gate) {
    return static_cast<HostPublicationGateState>(
        static_cast<uint32_t>(gate.control.load(std::memory_order_acquire) & kGateStateMask));
}

void StoreGateState(HostPublicationGate* gate, HostPublicationGateState state) {
    if (gate == nullptr) return;
    uint64_t current = gate->control.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t replacement = (current & ~kGateStateMask) | static_cast<uint32_t>(state);
        if (gate->control.compare_exchange_weak(current, replacement, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            return;
        }
    }
}

}  // namespace

bool HostBindingSupportsPublishedHooks(const HostApiBinding* binding) {
    if (binding == nullptr) return false;
    switch (binding->publication_policy) {
    case HostPublicationPolicy::kStrict:
        return binding->hook_with_publication != nullptr;
    case HostPublicationPolicy::kLocalGate:
        return binding->hook != nullptr;
    case HostPublicationPolicy::kConservative:
        return false;
    }
    return false;
}

DartPlantStatus PreparePublishedHostHook(PublishedHostHook* published,
                                         const HostApiBinding* binding, uintptr_t target,
                                         void* callback_entry, bool track_generated_entrants,
                                         void* testing_gate_entry) {
    if (published == nullptr || binding == nullptr || target == 0 || callback_entry == nullptr ||
        binding->unhook == nullptr || !HostBindingSupportsPublishedHooks(binding)) {
        SetLastError("published host hook arguments or host publication policy are invalid");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    published->binding = binding;
    published->target = target;
    published->track_generated_entrants = track_generated_entrants;
    published->gate.callback_entry = callback_entry;
    published->gate.backup = nullptr;
    published->gate.target = reinterpret_cast<void*>(target);
    published->gate.control.store(EncodeGateControl(HostPublicationGateState::kInstalling),
                                  std::memory_order_relaxed);
    if (testing_gate_entry != nullptr) {
        published->gate_entry = testing_gate_entry;
        published->gate_entry_size = 0;
        return DARTPLANT_OK;
    }
    published->gate_entry = CreateArm64HostPublicationGateStub(
        &published->gate, target, track_generated_entrants, &published->gate_entry_size);
    if (published->gate_entry == nullptr) {
        SetLastError("failed to allocate ARM64 host publication gate");
        return DARTPLANT_HOOK_FAILED;
    }
    return DARTPLANT_OK;
}

DartPlantStatus InstallPublishedHostHook(PublishedHostHook* published, void** out_backup) {
    if (published == nullptr || published->binding == nullptr || published->gate_entry == nullptr ||
        published->target == 0 || out_backup == nullptr) {
        SetLastError("published host hook is not prepared");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    *out_backup = nullptr;
    const HostApiBinding* binding = published->binding;
    void* backup = nullptr;
    int status = DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
    if (binding->publication_policy == HostPublicationPolicy::kStrict) {
        const auto backup_ready = [](void* user_data, void* ready_backup) {
            auto* current = static_cast<PublishedHostHook*>(user_data);
            if (current == nullptr || ready_backup == nullptr) return;
            current->gate.backup = ready_backup;
        };
        DartPlantHostHookTransaction transaction{
            .struct_size = sizeof(DartPlantHostHookTransaction),
            .user_data = published,
            .backup_ready = backup_ready,
        };
        status = binding->hook_with_publication(binding->user_data,
                                                reinterpret_cast<void*>(published->target),
                                                published->gate_entry, &transaction);
        backup = published->gate.backup;
    } else if (binding->publication_policy == HostPublicationPolicy::kLocalGate) {
        // The replacement can become reachable before this synchronous legacy
        // hook() returns. It is only the DartPlant gate, which remains closed in
        // kInstalling until backup has been copied below.
        status = binding->hook(binding->user_data, reinterpret_cast<void*>(published->target),
                               published->gate_entry, &backup);
        published->gate.backup = backup;
    }

    if (status != 0) {
        if (binding->publication_policy == HostPublicationPolicy::kStrict &&
            status == DARTPLANT_HOST_HOOK_FAILED_AFTER_PUBLISHED) {
            published->ever_published = true;
            published->backend_installed = false;
            StoreGateState(&published->gate, HostPublicationGateState::kBypassTarget);
        } else {
            StoreGateState(&published->gate, HostPublicationGateState::kFailed);
        }
        SetLastError("host hook function failed during published hook installation");
        return DARTPLANT_HOOK_FAILED;
    }
    if (backup == nullptr) {
        SetLastError("host hook succeeded without an original trampoline");
        __builtin_trap();
    }
    published->gate.backup = backup;
    published->backend_installed = true;
    published->ever_published = true;
    *out_backup = backup;
    return DARTPLANT_OK;
}

void ArmPublishedHostHook(PublishedHostHook* published) {
    if (published == nullptr || !published->backend_installed || published->gate.backup == nullptr)
        return;
    StoreGateState(&published->gate, HostPublicationGateState::kArmed);
}

void BypassPublishedHostHookToBackup(PublishedHostHook* published) {
    if (published == nullptr || published->gate.backup == nullptr) return;
    StoreGateState(&published->gate, HostPublicationGateState::kBypassBackup);
}

void BeginDrainPublishedHostHook(PublishedHostHook* published) {
    if (published == nullptr || !published->backend_installed) return;
    uint64_t current = published->gate.control.load(std::memory_order_acquire);
    for (;;) {
        const auto state = static_cast<HostPublicationGateState>(static_cast<uint32_t>(current));
        if (state == HostPublicationGateState::kDraining ||
            state == HostPublicationGateState::kClosed) {
            return;
        }
        if (state != HostPublicationGateState::kArmed) return;
        const uint64_t replacement = (current & ~kGateStateMask) |
                                     static_cast<uint32_t>(HostPublicationGateState::kDraining);
        if (published->gate.control.compare_exchange_weak(
                current, replacement, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
    }
}

bool ClosePublishedHostHook(PublishedHostHook* published) {
    if (published == nullptr || !published->backend_installed) return false;
    uint64_t current = published->gate.control.load(std::memory_order_acquire);
    for (;;) {
        const auto state = static_cast<HostPublicationGateState>(static_cast<uint32_t>(current));
        if (state == HostPublicationGateState::kClosed) return (current >> 32) == 0;
        // Installation rollback may close an unpublished logical callback
        // directly: the legacy backend can already own target, but kInstalling
        // has never permitted a CPU to cross the gate.
        if (state != HostPublicationGateState::kDraining &&
            state != HostPublicationGateState::kInstalling) {
            return false;
        }
        if ((current >> 32) != 0) return false;
        const uint64_t replacement = static_cast<uint32_t>(HostPublicationGateState::kClosed);
        if (published->gate.control.compare_exchange_weak(
                current, replacement, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
}

void ReopenPublishedHostHookDrain(PublishedHostHook* published) {
    if (published == nullptr || !published->backend_installed) return;
    uint64_t expected = static_cast<uint32_t>(HostPublicationGateState::kClosed);
    (void) published->gate.control.compare_exchange_strong(
        expected, static_cast<uint32_t>(HostPublicationGateState::kDraining),
        std::memory_order_acq_rel, std::memory_order_acquire);
}

bool PublishedHostHookEntrantsIdle(const PublishedHostHook* published) {
    return PublishedHostHookEntrantCount(published) == 0;
}

uint32_t PublishedHostHookEntrantCount(const PublishedHostHook* published) {
    if (published == nullptr) return 0;
    return static_cast<uint32_t>(published->gate.control.load(std::memory_order_acquire) >> 32);
}

DartPlantStatus UninstallPublishedHostHook(PublishedHostHook* published) {
    if (published == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    if (!published->backend_installed) {
        StoreGateState(&published->gate, HostPublicationGateState::kBypassTarget);
        return DARTPLANT_OK;
    }
    if (!PublishedHostHookEntrantsIdle(published)) {
        SetLastError("published host hook still has entry handoff pins");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    if (!ClosePublishedHostHook(published)) {
        SetLastError("published host hook is not closed for physical unhook");
        return DARTPLANT_VM_ADAPTER_BUSY;
    }
    const HostApiBinding* binding = published->binding;
    if (binding == nullptr || binding->unhook == nullptr ||
        binding->unhook(binding->user_data, reinterpret_cast<void*>(published->target)) != 0) {
        ReopenPublishedHostHookDrain(published);
        SetLastError("host unhook function failed");
        return DARTPLANT_UNHOOK_FAILED;
    }
    published->backend_installed = false;
    StoreGateState(&published->gate, HostPublicationGateState::kBypassTarget);
    return DARTPLANT_OK;
}

void DestroyPublishedHostHookGate(PublishedHostHook* published) {
    if (published == nullptr || published->gate_entry == nullptr || published->ever_published)
        return;
    DestroyArm64CallbackStub(published->gate_entry, published->gate_entry_size);
    published->gate_entry = nullptr;
    published->gate_entry_size = 0;
}

void RetainPublishedHostHookForProcessLifetime(std::unique_ptr<PublishedHostHook> published) {
    if (published == nullptr) return;
    // A published gate stub embeds &published->gate as its dispatcher cookie.
    // Without a host-provided instruction-fetch grace period there is no point
    // at which a FAILED_AFTER_PUBLISHED owner can prove that another CPU has
    // not already fetched that gate. Releasing the unique_ptr would therefore
    // leave a live executable page naming freed data. Intentionally leak the
    // small owner together with its already-retained gate page.
    (void) published.release();
}

void ReleasePublishedHostHookEntrant(PublishedHostHook* published) {
    if (published == nullptr || !published->track_generated_entrants) return;
    uint64_t current = published->gate.control.load(std::memory_order_acquire);
    while ((current >> 32) != 0) {
        if (published->gate.control.compare_exchange_weak(current, current - kGateEntrantOne,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
            return;
        }
    }
}

void ReleasePublishedHostHookEntrant(DartPlantHook* hook) {
    if (hook == nullptr) return;
    ReleasePublishedHostHookEntrant(hook->published_entry_hook.get());
}

void* AcquirePublishedHostHookRouteForTesting(PublishedHostHook* published,
                                              bool* out_callback_pin) {
    if (out_callback_pin != nullptr) *out_callback_pin = false;
    if (published == nullptr) return nullptr;
    for (;;) {
        const HostPublicationGateState state = LoadGateState(published->gate);
        if (state == HostPublicationGateState::kInstalling ||
            state == HostPublicationGateState::kClosed) {
            std::this_thread::yield();
            continue;
        }
        if (state == HostPublicationGateState::kBypassTarget) return published->gate.target;
        if (state == HostPublicationGateState::kBypassBackup) return published->gate.backup;
        if (state != HostPublicationGateState::kArmed &&
            state != HostPublicationGateState::kDraining) {
            return nullptr;
        }

        if (!published->track_generated_entrants) return published->gate.callback_entry;

        uint64_t control = published->gate.control.load(std::memory_order_acquire);
        for (;;) {
            const auto control_state =
                static_cast<HostPublicationGateState>(static_cast<uint32_t>(control));
            if (control_state != HostPublicationGateState::kArmed &&
                control_state != HostPublicationGateState::kDraining) {
                break;
            }
            if ((control >> 32) == UINT32_MAX) return nullptr;
            if (published->gate.control.compare_exchange_weak(control, control + kGateEntrantOne,
                                                              std::memory_order_acq_rel,
                                                              std::memory_order_acquire)) {
                if (out_callback_pin != nullptr) *out_callback_pin = true;
                return published->gate.callback_entry;
            }
        }
    }
}

}  // namespace dartplant
