// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_DARTPLANT_H_
#define DARTPLANT_DARTPLANT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define DARTPLANT_EXPORT __attribute__((visibility("default")))
#else
#define DARTPLANT_EXPORT
#endif

typedef struct DartPlantMethod DartPlantMethod;
typedef struct DartPlantHook DartPlantHook;
typedef struct DartPlantListener DartPlantListener;

typedef enum DartPlantStatus {
    DARTPLANT_OK = 0,
    DARTPLANT_INVALID_ARGUMENT,
    DARTPLANT_NOT_INITIALIZED,
    DARTPLANT_HOST_API_UNAVAILABLE,
    DARTPLANT_METADATA_INVALID,
    DARTPLANT_MODULE_NOT_FOUND,
    DARTPLANT_METHOD_NOT_FOUND,
    DARTPLANT_AMBIGUOUS_METHOD,
    DARTPLANT_BUILD_ID_MISMATCH,
    DARTPLANT_FINGERPRINT_MISMATCH,
    DARTPLANT_ADDRESS_OUTSIDE_EXECUTABLE,
    DARTPLANT_UNSUPPORTED_ADDRESS_KIND,
    DARTPLANT_UNSUPPORTED_ABI,
    DARTPLANT_PROFILE_MISMATCH,
    DARTPLANT_RUNTIME_NOT_READY,
    DARTPLANT_INVALID_INVOCATION_PHASE,
    DARTPLANT_ALREADY_HOOKED,
    DARTPLANT_HOOK_FAILED,
    DARTPLANT_UNHOOK_FAILED,
    DARTPLANT_VM_BRIDGE_UNAVAILABLE,
    DARTPLANT_VM_ISOLATE_MISMATCH,
    DARTPLANT_VM_THREAD_MISMATCH,
    DARTPLANT_VM_SCOPE_REQUIRED,
    DARTPLANT_VM_ADAPTER_BUSY,
    DARTPLANT_OBJECT_HANDLE_INVALID,
    DARTPLANT_OBJECT_COLLECTED,
    DARTPLANT_SHARED_CODE_ENTRY,
} DartPlantStatus;

typedef enum DartPlantAddressKind {
    DARTPLANT_ADDRESS_RUNTIME = 0,
    DARTPLANT_ADDRESS_ELF_VA,
    DARTPLANT_ADDRESS_FILE_OFFSET,
    DARTPLANT_ADDRESS_SNAPSHOT_OFFSET,
} DartPlantAddressKind;

typedef enum DartPlantEntryKind {
    DARTPLANT_ENTRY_DEFAULT = 0,
    DARTPLANT_ENTRY_UNCHECKED,
    DARTPLANT_ENTRY_MONOMORPHIC,
    DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED,
} DartPlantEntryKind;

typedef struct DartPlantMethodQuery {
    uint32_t struct_size;
    const char* library_uri;
    const char* class_name;
    const char* function_name;
    const char* signature;
    DartPlantEntryKind entry_kind;
} DartPlantMethodQuery;

typedef struct DartPlantAddressQuery {
    uint32_t struct_size;
    const char* module_name;
    uint64_t address;
    DartPlantAddressKind address_kind;
    uint32_t code_size;
    const char* expected_build_id;
    const char* expected_fingerprint;
} DartPlantAddressQuery;

DARTPLANT_EXPORT DartPlantStatus dartplant_initialize_from_json(const char* metadata_json);
DARTPLANT_EXPORT void dartplant_reset(void);
DARTPLANT_EXPORT const char* dartplant_last_error(void);

DARTPLANT_EXPORT DartPlantStatus dartplant_find_method(const DartPlantMethodQuery* query,
                                                       DartPlantMethod** out_method);
DARTPLANT_EXPORT void dartplant_release_method(DartPlantMethod* method);
DARTPLANT_EXPORT uintptr_t dartplant_method_runtime_address(const DartPlantMethod* method);

DARTPLANT_EXPORT DartPlantStatus dartplant_hook_method_raw(const DartPlantMethod* method,
                                                           void* replacement, void** backup,
                                                           DartPlantHook** out_hook);
DARTPLANT_EXPORT DartPlantStatus dartplant_hook_address(const DartPlantAddressQuery* query,
                                                        void* replacement, void** backup,
                                                        DartPlantHook** out_hook);
DARTPLANT_EXPORT DartPlantStatus dartplant_unhook(DartPlantHook* hook);
DARTPLANT_EXPORT uint8_t dartplant_is_hooked(const DartPlantMethod* method);
DARTPLANT_EXPORT void dartplant_release_hook(DartPlantHook* hook);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_DARTPLANT_H_
