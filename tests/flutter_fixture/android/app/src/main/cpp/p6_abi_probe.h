// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

int32_t dartplant_fixture_p6_abi_install();
uint64_t dartplant_fixture_p6_abi_probe();
void dartplant_fixture_p6_abi_cleanup();
int32_t dartplant_fixture_exception_bridge_lifetime_install();
uint64_t dartplant_fixture_exception_bridge_lifetime_probe();
void dartplant_fixture_exception_bridge_lifetime_cleanup();

#ifdef __cplusplus
}
#endif
