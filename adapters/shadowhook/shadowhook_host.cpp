// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <shadowhook.h>

#include <mutex>
#include <optional>
#include <unordered_map>

#include "dartplant/adapters/shadowhook.h"

namespace {

struct ShadowHookState {
    std::mutex mutex;
    std::unordered_map<void*, void*> stubs;
};

ShadowHookState& State() {
    static ShadowHookState state;
    return state;
}

bool EnsureInitialized() {
    static std::once_flag once;
    static std::optional<int> result;
    std::call_once(once, [] { result = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false); });
    return result.has_value() && *result == 0;
}

int Hook(void* user_data, void* target, void* replacement, void** backup) {
    auto* state = static_cast<ShadowHookState*>(user_data);
    if (state == nullptr || target == nullptr || replacement == nullptr || backup == nullptr) {
        return -1;
    }
    void* stub = shadowhook_hook_func_addr(target, replacement, backup);
    if (stub == nullptr) return -1;
    std::lock_guard lock(state->mutex);
    const auto [_, inserted] = state->stubs.emplace(target, stub);
    if (!inserted) {
        shadowhook_unhook(stub);
        return -1;
    }
    return 0;
}

int Unhook(void* user_data, void* target) {
    auto* state = static_cast<ShadowHookState*>(user_data);
    if (state == nullptr || target == nullptr) return -1;
    void* stub = nullptr;
    {
        std::lock_guard lock(state->mutex);
        const auto found = state->stubs.find(target);
        if (found == state->stubs.end()) return -1;
        stub = found->second;
    }
    if (shadowhook_unhook(stub) != 0) return -1;
    std::lock_guard lock(state->mutex);
    state->stubs.erase(target);
    return 0;
}

const DartPlantHostApi kShadowHookHostApi = {
    .struct_size = sizeof(DartPlantHostApi),
    .version = DARTPLANT_HOST_API_VERSION,
    .user_data = &State(),
    .hook = Hook,
    .unhook = Unhook,
};

}  // namespace

extern "C" const DartPlantHostApi* dartplant_shadowhook_host_api(void) {
    return EnsureInitialized() ? &kShadowHookHostApi : nullptr;
}
