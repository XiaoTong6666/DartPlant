// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ADAPTERS_SHADOWHOOK_H_
#define DARTPLANT_ADAPTERS_SHADOWHOOK_H_

#include "dartplant/host_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initializes ShadowHook in unique mode once and returns the process-lifetime
// HostApi binding. Returns null if ShadowHook initialization fails.
const DartPlantHostApi* dartplant_shadowhook_host_api(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_ADAPTERS_SHADOWHOOK_H_
