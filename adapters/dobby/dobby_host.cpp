// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dobby.h>

#include "dartplant/adapters/dobby.h"

namespace {

int Hook(void*, void* target, void* replacement, void** backup) {
    return DobbyHook(target, reinterpret_cast<dobby_dummy_func_t>(replacement),
                     reinterpret_cast<dobby_dummy_func_t*>(backup));
}

int Unhook(void*, void* target) { return DobbyDestroy(target); }

const DartPlantHostApi kDobbyHostApi = {
    .struct_size = sizeof(DartPlantHostApi),
    .version = DARTPLANT_HOST_API_VERSION,
    .user_data = nullptr,
    .hook = Hook,
    .unhook = Unhook,
};

}  // namespace

extern "C" const DartPlantHostApi* dartplant_dobby_host_api(void) { return &kDobbyHostApi; }
