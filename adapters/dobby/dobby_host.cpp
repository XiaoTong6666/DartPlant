// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dobby.h>

#include <mutex>
#include <unordered_map>
#include "dartplant/adapters/dobby.h"

namespace {

#if defined(DOBBY_HOOK_TRANSACTION_API_VERSION)
std::mutex g_owned_mutex;
enum class PhysicalState : uint8_t { kPreparing, kInstalled, kRecoveryRequired };
struct OwnedTransaction {
    DobbyHookHandle ticket = 0;
    PhysicalState state = PhysicalState::kPreparing;
};
std::unordered_map<void*, OwnedTransaction> g_owned_hooks;

// Both strict and ordinary calls enter the same Dobby transaction manager.
// Reserve our ownership mapping before Commit, so an uncertain failure can be
// recovered by the host's original unhook callback rather than calling a
// foreign registry entry at this address.
int Install(void* target, void* replacement, void** backup,
            const DartPlantHostHookTransaction* publication) {
    if (backup != nullptr) *backup = nullptr;
    if (target == nullptr || replacement == nullptr ||
        (publication != nullptr &&
         (publication->struct_size < sizeof(*publication) ||
          publication->backup_ready == nullptr))) {
        return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
    }
    std::unique_lock lock(g_owned_mutex);
    if (g_owned_hooks.find(target) != g_owned_hooks.end())
        return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;

    DobbyHookOptions options{
        sizeof(DobbyHookOptions),
#if defined(__aarch64__)
        DOBBY_BRANCH_REQUIRE_NEAR,
        DOBBY_HOOK_REQUIRE_CONCURRENT_SAFE,
#else
        DOBBY_BRANCH_LEGACY,
        0,
#endif
        0,
        target,
        reinterpret_cast<dobby_dummy_func_t>(replacement),
    };
    DobbyHookResult result{};
    result.struct_size = sizeof(result);
    if (DobbyPrepareHook(&options, &result) != RS_SUCCESS)
        return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;

    const DobbyHookHandle ticket = result.handle;
    if (ticket == 0 || result.original == nullptr) {
        // A malformed "successful" backend result must not leave a reserved
        // address behind. Never hand an unpublished backup to the caller.
        if (ticket != 0) {
            DobbyHookResult aborted{};
            aborted.struct_size = sizeof(aborted);
            (void) DobbyAbortHook(ticket, &aborted);
        }
        return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
    }
    try {
        g_owned_hooks.emplace(target, OwnedTransaction{ticket, PhysicalState::kPreparing});
    } catch (...) {
        DobbyHookResult abort{};
        abort.struct_size = sizeof(abort);
        (void) DobbyAbortHook(ticket, &abort);
        return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
    }
    lock.unlock(); // Never run a publication callback or Commit under our map lock.
    // Publication of a callable original precedes the first physical entry
    // write. For the strict host the real callback remains behind DartPlant's
    // closed gate until the entire HookRecord is ready.
    if (backup != nullptr) *backup = reinterpret_cast<void*>(result.original);
    if (publication != nullptr)
        publication->backup_ready(publication->user_data, reinterpret_cast<void*>(result.original));
    if (DobbyCommitHook(ticket, &result) == RS_SUCCESS) {
        lock.lock();
        auto it = g_owned_hooks.find(target);
        if (it != g_owned_hooks.end() && it->second.ticket == ticket)
            it->second.state = PhysicalState::kInstalled;
        return RS_SUCCESS;
    }

    // A changed prologue is a PREPARED failure, not a committed one. Dobby
    // intentionally keeps its ticket so Abort can release the reservation
    // without touching another writer's bytes. A failed Commit does not
    // necessarily mean the backend released the ticket.
    if (result.handle == ticket && !result.ever_published &&
        !result.target_may_be_patched &&
        result.status != DOBBY_HOOK_RECOVERY_REQUIRED) {
        DobbyHookResult aborted{};
        aborted.struct_size = sizeof(aborted);
        if (DobbyAbortHook(ticket, &aborted) == RS_SUCCESS)
            result.handle = 0;
    }
    lock.lock();
    if (result.handle == 0) {
        g_owned_hooks.erase(target);
    } else {
        auto it = g_owned_hooks.find(target);
        if (it != g_owned_hooks.end() && it->second.ticket == ticket)
            it->second.state = PhysicalState::kRecoveryRequired;
    }
    if (result.handle != 0)
        return DARTPLANT_HOST_HOOK_FAILED_RECOVERY_REQUIRED;
    if (result.ever_published)
        return DARTPLANT_HOST_HOOK_FAILED_AFTER_PUBLISHED;
    if (backup != nullptr) *backup = nullptr;
    return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
}

int Hook(void*, void* target, void* replacement, void** backup) {
    return Install(target, replacement, backup, nullptr);
}

int HookWithPublication(void*, void* target, void* replacement,
                        DartPlantHostHookTransaction* transaction) {
    return Install(target, replacement, nullptr, transaction);
}

int Unhook(void*, void* target) {
    std::lock_guard lock(g_owned_mutex);
    const auto it = g_owned_hooks.find(target);
    if (it == g_owned_hooks.end()) return RS_FAILED;
    // A reentrant unhook from backup_ready (or another thread during Commit)
    // cannot steal or erase the still-preparing transaction.
    if (it->second.state == PhysicalState::kPreparing) return RS_FAILED;
    DobbyHookResult result{};
    result.struct_size = sizeof(result);
    const int status = it->second.state == PhysicalState::kRecoveryRequired
        ? DobbyRecoverHook(it->second.ticket, &result)
        : DobbyDestroyHook(it->second.ticket, &result);
    if (status != RS_SUCCESS)
        return RS_FAILED; // Retain the original ticket on an uncertain patch.
    g_owned_hooks.erase(it);
    return RS_SUCCESS;
}
#else
// Old vendored Dobby has no safe publication or physical ownership receipt.
// A nonzero DobbyHook could mean "target was owned by somebody else" OR
// "partial commit still installed". Calling DobbyDestroy(target) on either
// guess could uninstall another module's hook. Fail closed and request the
// new fork rather than making that unprovable assumption.
int Hook(void*, void* target, void* replacement, void** backup) {
    (void) target;
    (void) replacement;
    if (backup != nullptr) *backup = nullptr;
    return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
}

int Unhook(void*, void*) { return RS_FAILED; }
#endif

const DartPlantHostApi kDobbyHostApi = {
    .struct_size = sizeof(DartPlantHostApi),
    .version = DARTPLANT_HOST_API_VERSION,
    .user_data = nullptr,
    .hook = Hook,
    .unhook = Unhook,
#if defined(DOBBY_HOOK_TRANSACTION_API_VERSION)
    .hook_with_publication = HookWithPublication,
#else
    .hook_with_publication = nullptr,
#endif
};

}  // namespace

extern "C" const DartPlantHostApi* dartplant_dobby_host_api(void) { return &kDobbyHostApi; }
