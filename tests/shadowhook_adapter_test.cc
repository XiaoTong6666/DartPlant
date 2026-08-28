// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include "dartplant/adapters/shadowhook.h"
#include "dartplant/host_api.h"
#include "shadowhook.h"
#include "test_runner.h"

namespace {

int g_init_calls = 0;
shadowhook_mode_t g_init_mode = SHADOWHOOK_MODE_SHARED;
int g_hook_calls = 0;
uint32_t g_hook_flags = 0;
void* g_hook_target = nullptr;
void* g_hook_replacement = nullptr;
void* g_stub = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12340000));
void* g_backup = reinterpret_cast<void*>(static_cast<uintptr_t>(0x56780000));
int g_unhook_calls = 0;
void* g_unhook_stub = nullptr;

}  // namespace

extern "C" int shadowhook_init(shadowhook_mode_t default_mode, bool) {
    ++g_init_calls;
    g_init_mode = default_mode;
    // Simulate a process where ShadowHook is already initialized. Returning
    // success says nothing about the actual global default mode; the adapter
    // must still force UNIQUE in the per-hook flags.
    return 0;
}

extern "C" void* shadowhook_hook_func_addr_2(void* func_addr, void* new_addr, void** orig_addr,
                                             uint32_t flags, ...) {
    ++g_hook_calls;
    g_hook_flags = flags;
    g_hook_target = func_addr;
    g_hook_replacement = new_addr;
    if (orig_addr != nullptr) *orig_addr = g_backup;
    return g_stub;
}

extern "C" int shadowhook_unhook(void* stub) {
    ++g_unhook_calls;
    g_unhook_stub = stub;
    return 0;
}

TEST_CASE(ShadowHookAdapterForcesUniqueModePerPhysicalHook) {
    const DartPlantHostApi* api = dartplant_shadowhook_host_api();
    EXPECT_TRUE(api != nullptr);
    EXPECT_EQ(1, g_init_calls);
    EXPECT_EQ(static_cast<int>(SHADOWHOOK_MODE_UNIQUE), static_cast<int>(g_init_mode));

    void* target = reinterpret_cast<void*>(static_cast<uintptr_t>(0x11110000));
    void* replacement = reinterpret_cast<void*>(static_cast<uintptr_t>(0x22220000));
    void* backup = nullptr;
    EXPECT_EQ(0, api->hook(api->user_data, target, replacement, &backup));
    EXPECT_EQ(1, g_hook_calls);
    EXPECT_EQ(static_cast<uint32_t>(SHADOWHOOK_HOOK_WITH_UNIQUE_MODE), g_hook_flags);
    EXPECT_EQ(target, g_hook_target);
    EXPECT_EQ(replacement, g_hook_replacement);
    EXPECT_EQ(g_backup, backup);

    EXPECT_EQ(0, api->unhook(api->user_data, target));
    EXPECT_EQ(1, g_unhook_calls);
    EXPECT_EQ(g_stub, g_unhook_stub);
}
