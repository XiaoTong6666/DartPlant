// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ADAPTERS_DOBBY_H_
#define DARTPLANT_ADAPTERS_DOBBY_H_

#include "dartplant/host_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns a process-lifetime HostApi backed by DobbyHook/DobbyDestroy.
// Dobby remains an optional consumer-selected dependency and is never linked
// into dartplant_core.
const DartPlantHostApi* dartplant_dobby_host_api(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_ADAPTERS_DOBBY_H_
