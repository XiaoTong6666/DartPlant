// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>

#include "core/internal.h"
#include "test_runner.h"

TEST_CASE(EnumerateModulesFindsLoadedFixture) {
    void* handle = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(handle != nullptr);
    void* symbol = dlsym(handle, "DartPlantFixtureAdd");
    EXPECT_TRUE(symbol != nullptr);

    const auto modules = dartplant::EnumerateModules();
    const auto module = dartplant::FindModule(modules, "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());
    EXPECT_TRUE(module->load_bias != 0);
    EXPECT_TRUE(module->ContainsExecutable(reinterpret_cast<uintptr_t>(symbol), 1));

    const uintptr_t address = reinterpret_cast<uintptr_t>(symbol);
    const auto resolved = module->Resolve(DARTPLANT_ADDRESS_ELF_VA, address - module->load_bias);
    EXPECT_TRUE(resolved.has_value());
    EXPECT_EQ(address, *resolved);
    dlclose(handle);
}

TEST_CASE(FingerprintIsStableAndSensitive) {
    const unsigned char first[] = {1, 2, 3, 4};
    const unsigned char second[] = {1, 2, 3, 5};
    EXPECT_EQ(dartplant::FingerprintCode(first, sizeof(first)),
              dartplant::FingerprintCode(first, sizeof(first)));
    EXPECT_FALSE(dartplant::FingerprintCode(first, sizeof(first)) ==
                 dartplant::FingerprintCode(second, sizeof(second)));
}
