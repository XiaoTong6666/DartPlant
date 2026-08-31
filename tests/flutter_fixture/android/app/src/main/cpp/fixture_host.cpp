// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "fixture_host.h"

#include <dobby.h>

#include "core/internal.h"
#include "dartplant/host_api.h"

namespace dartplant_fixture {
namespace {

int LegacyHook(void*, void* target, void* replacement, void** backup) {
    return DobbyHook(target, reinterpret_cast<dobby_dummy_func_t>(replacement),
                     reinterpret_cast<dobby_dummy_func_t*>(backup));
}

int LegacyUnhook(void*, void* target) { return DobbyDestroy(target); }

const DartPlantHostApi kLegacyHostApi = {
    .struct_size = sizeof(DartPlantHostApi),
    .version = DARTPLANT_HOST_API_VERSION,
    .user_data = nullptr,
    .hook = LegacyHook,
    .unhook = LegacyUnhook,
    .hook_with_publication = nullptr,
};

}  // namespace

DartPlantStatus InstallLocalGateHost() {
    dartplant::InstallHostApi(&kLegacyHostApi, dartplant::HostPublicationPolicy::kLocalGate);
    const auto* binding = dartplant::State().host.binding.load(std::memory_order_acquire);
    if (binding == nullptr || binding->hook != kLegacyHostApi.hook ||
        binding->unhook != kLegacyHostApi.unhook ||
        binding->publication_policy != dartplant::HostPublicationPolicy::kLocalGate) {
        dartplant::SetLastError("Flutter fixture failed to install local-gate legacy host");
        return DARTPLANT_HOST_API_UNAVAILABLE;
    }
    return DARTPLANT_OK;
}

}  // namespace dartplant_fixture
