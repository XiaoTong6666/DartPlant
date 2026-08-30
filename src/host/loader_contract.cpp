// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>

#include "core/internal.h"
#include "dartplant/advanced/loader.h"

extern "C" DartPlantStatus dartplant_loader_get_capabilities(
    DartPlantLoaderCapabilities* out_capabilities) {
    if (out_capabilities == nullptr ||
        out_capabilities->struct_size < sizeof(DartPlantLoaderCapabilities)) {
        dartplant::SetLastError("loader capability output is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    DartPlantLoaderCapabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
#if defined(DARTPLANT_USE_PTHREAD_TLS)
    capabilities.tls_backend = DARTPLANT_TLS_PTHREAD_KEY;
#else
    capabilities.tls_backend = DARTPLANT_TLS_NATIVE;
#endif
    capabilities.explicit_handle_symbol_lookup = 1;
#if defined(DARTPLANT_MINIMAL_LINKER_COMPAT)
    capabilities.rtld_default_lookup_safe = 0;
#else
    capabilities.rtld_default_lookup_safe = 1;
#endif
    // Live VM bootstrap borrows only an otherwise-default SIGWINCH/SIGURG and
    // restores it. DartPlant deliberately does not install process-global
    // SIGSEGV/SIGBUS probing handlers like FlutterTap's safe_read path.
    capabilities.temporary_signal_lease = 1;
    capabilities.installs_fault_signal_handlers = 0;
    *out_capabilities = capabilities;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

extern "C" DartPlantStatus dartplant_loader_resolve_loaded_symbol(
    const char* library_name, const char* symbol_name, uint8_t allow_rtld_default_fallback,
    void** out_symbol, DartPlantSymbolLookupPath* out_path) {
    if (symbol_name == nullptr || symbol_name[0] == '\0' || out_symbol == nullptr) {
        dartplant::SetLastError("loader symbol lookup arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    *out_symbol = nullptr;
    if (out_path != nullptr) *out_path = DARTPLANT_SYMBOL_LOOKUP_NONE;

    if (library_name != nullptr && library_name[0] != '\0') {
        if (void* handle = dlopen(library_name, RTLD_NOW | RTLD_NOLOAD); handle != nullptr) {
            void* symbol = dlsym(handle, symbol_name);
            dlclose(handle);
            if (symbol != nullptr) {
                *out_symbol = symbol;
                if (out_path != nullptr) *out_path = DARTPLANT_SYMBOL_LOOKUP_EXPLICIT_HANDLE;
                dartplant::ClearLastError();
                return DARTPLANT_OK;
            }
        }
    }

    if (allow_rtld_default_fallback != 0) {
#if defined(DARTPLANT_MINIMAL_LINKER_COMPAT)
        dartplant::SetLastError(
            "RTLD_DEFAULT fallback is disabled by the minimal/custom-loader contract");
        return DARTPLANT_METHOD_NOT_FOUND;
#else
        if (void* symbol = dlsym(RTLD_DEFAULT, symbol_name); symbol != nullptr) {
            *out_symbol = symbol;
            if (out_path != nullptr) *out_path = DARTPLANT_SYMBOL_LOOKUP_RTLD_DEFAULT;
            dartplant::ClearLastError();
            return DARTPLANT_OK;
        }
#endif
    }

    dartplant::SetLastError("symbol was not found in the requested loaded-library scope");
    return DARTPLANT_METHOD_NOT_FOUND;
}
