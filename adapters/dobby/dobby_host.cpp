// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dobby.h>

#include "InterceptRouting/Routing/FunctionInlineHook/FunctionInlineHookRouting.h"
#include "Interceptor.h"
#include "dartplant/adapters/dobby.h"
#include "dobby_internal.h"

namespace {

int Hook(void*, void* target, void* replacement, void** backup) {
    const int status = DobbyHook(target, reinterpret_cast<dobby_dummy_func_t>(replacement),
                                 reinterpret_cast<dobby_dummy_func_t*>(backup));
    if (status != RS_SUCCESS) return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
    if (backup != nullptr && *backup != nullptr) return RS_SUCCESS;
    // Dobby has committed this entry, so this adapter owns its rollback. A
    // failed rollback leaves no callable original path for DartPlant to use.
    if (DobbyDestroy(target) != RS_SUCCESS) __builtin_trap();
    return DARTPLANT_HOST_HOOK_FAILED_AFTER_PUBLISHED;
}

int HookWithPublication(void* user_data, void* target, void* replacement,
                        DartPlantHostHookTransaction* transaction) {
    if (transaction == nullptr || transaction->struct_size < sizeof(*transaction) ||
        transaction->backup_ready == nullptr) {
        return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
    }
    if (target == nullptr || replacement == nullptr) {
        return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
    }
#if defined(ANDROID)
    void* page_align_address = (void*) ALIGN_FLOOR(target, OSMemory::PageSize());
    if (!OSMemory::SetPermission(page_align_address, OSMemory::PageSize(), kReadExecute)) {
        return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
    }
#endif
    auto* entry = Interceptor::SharedInstance()->find((addr_t) target);
    if (entry != nullptr) return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;

    entry = new InterceptEntry(kFunctionInlineHook, (addr_t) target);
    auto* routing =
        new FunctionInlineHookRouting(entry, reinterpret_cast<dobby_dummy_func_t>(replacement));
    routing->Prepare();
    routing->DispatchRouting();
    if (entry->relocated_addr == 0) return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;

    // Dobby's public DobbyHook writes origin_func before Commit(). This strict
    // adapter preserves that ordering and lets DartPlant publish backup to the
    // callback stub before the first instruction patch becomes reachable.
    transaction->backup_ready(transaction->user_data,
                              reinterpret_cast<void*>(entry->relocated_addr));
    if (!routing->Commit()) {
        return DARTPLANT_HOST_HOOK_FAILED_NEVER_PUBLISHED;
    }
    Interceptor::SharedInstance()->add(entry);
    (void) user_data;
    return RS_SUCCESS;
}

int Unhook(void*, void* target) { return DobbyDestroy(target); }

const DartPlantHostApi kDobbyHostApi = {
    .struct_size = sizeof(DartPlantHostApi),
    .version = DARTPLANT_HOST_API_VERSION,
    .user_data = nullptr,
    .hook = Hook,
    .unhook = Unhook,
    .hook_with_publication = HookWithPublication,
};

}  // namespace

extern "C" const DartPlantHostApi* dartplant_dobby_host_api(void) { return &kDobbyHostApi; }
