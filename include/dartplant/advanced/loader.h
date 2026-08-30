// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ADVANCED_LOADER_H_
#define DARTPLANT_ADVANCED_LOADER_H_

#include <stdint.h>

#include "dartplant/dartplant.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DartPlantTlsBackend {
    DARTPLANT_TLS_NATIVE = 0,
    DARTPLANT_TLS_PTHREAD_KEY,
} DartPlantTlsBackend;

typedef enum DartPlantSymbolLookupPath {
    DARTPLANT_SYMBOL_LOOKUP_NONE = 0,
    DARTPLANT_SYMBOL_LOOKUP_EXPLICIT_HANDLE,
    DARTPLANT_SYMBOL_LOOKUP_RTLD_DEFAULT,
} DartPlantSymbolLookupPath;

typedef struct DartPlantLoaderCapabilities {
    uint32_t struct_size;
    DartPlantTlsBackend tls_backend;
    uint8_t explicit_handle_symbol_lookup;
    // True only when this image is intended to be known to the platform
    // dynamic linker. Minimal/custom-loader builds report false because
    // RTLD_DEFAULT may require a caller soinfo that does not exist.
    uint8_t rtld_default_lookup_safe;
    uint8_t temporary_signal_lease;
    uint8_t installs_fault_signal_handlers;
} DartPlantLoaderCapabilities;

// Describes the contract compiled into this DartPlant image. Minimal/custom
// loader builds use pthread keys and can be ELF-audited independently.
DARTPLANT_EXPORT DartPlantStatus
dartplant_loader_get_capabilities(DartPlantLoaderCapabilities* out_capabilities);

// Resolve a symbol with an explicit RTLD_NOLOAD library handle first. This
// avoids bionic's caller-soinfo lookup through RTLD_DEFAULT, which is not valid
// when DartPlant itself was mapped by a custom/minimal loader. Plain linker
// hosts may permit the RTLD_DEFAULT fallback.
DARTPLANT_EXPORT DartPlantStatus dartplant_loader_resolve_loaded_symbol(
    const char* library_name, const char* symbol_name, uint8_t allow_rtld_default_fallback,
    void** out_symbol, DartPlantSymbolLookupPath* out_path);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_ADVANCED_LOADER_H_
