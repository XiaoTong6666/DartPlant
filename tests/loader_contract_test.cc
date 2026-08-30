// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>

#include "dartplant/advanced/loader.h"
#include "test_runner.h"

TEST_CASE(LoaderContractPrefersExplicitLoadedLibraryScope) {
    void* handle = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(handle != nullptr);

    void* symbol = nullptr;
    DartPlantSymbolLookupPath path = DARTPLANT_SYMBOL_LOOKUP_NONE;
    EXPECT_EQ(DARTPLANT_OK, dartplant_loader_resolve_loaded_symbol(
                                DARTPLANT_FIXTURE_PATH, "DartPlantFixtureAdd", 0, &symbol, &path));
    EXPECT_TRUE(symbol != nullptr);
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_SYMBOL_LOOKUP_EXPLICIT_HANDLE),
              static_cast<uint8_t>(path));
    EXPECT_TRUE(symbol == dlsym(handle, "DartPlantFixtureAdd"));
    dlclose(handle);
}

TEST_CASE(LoaderContractReportsTlsAndSignalPolicy) {
    DartPlantLoaderCapabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    EXPECT_EQ(DARTPLANT_OK, dartplant_loader_get_capabilities(&capabilities));
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_TLS_NATIVE),
              static_cast<uint8_t>(capabilities.tls_backend));
    EXPECT_EQ(1U, capabilities.explicit_handle_symbol_lookup);
    EXPECT_EQ(1U, capabilities.rtld_default_lookup_safe);
    EXPECT_EQ(1U, capabilities.temporary_signal_lease);
    EXPECT_EQ(0U, capabilities.installs_fault_signal_handlers);
}

TEST_CASE(LoaderContractRtldDefaultFallbackIsExplicitOptIn) {
    void* symbol = nullptr;
    DartPlantSymbolLookupPath path = DARTPLANT_SYMBOL_LOOKUP_NONE;
    EXPECT_EQ(DARTPLANT_METHOD_NOT_FOUND,
              dartplant_loader_resolve_loaded_symbol("definitely-not-loaded.so", "malloc", 0,
                                                     &symbol, &path));
    EXPECT_TRUE(symbol == nullptr);
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_SYMBOL_LOOKUP_NONE), static_cast<uint8_t>(path));

    EXPECT_EQ(DARTPLANT_OK, dartplant_loader_resolve_loaded_symbol("definitely-not-loaded.so",
                                                                   "malloc", 1, &symbol, &path));
    EXPECT_TRUE(symbol != nullptr);
    EXPECT_EQ(static_cast<uint8_t>(DARTPLANT_SYMBOL_LOOKUP_RTLD_DEFAULT),
              static_cast<uint8_t>(path));
}
