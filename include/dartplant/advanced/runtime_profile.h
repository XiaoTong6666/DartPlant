// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ADVANCED_RUNTIME_PROFILE_H_
#define DARTPLANT_ADVANCED_RUNTIME_PROFILE_H_

#include <stdint.h>

#include "dartplant/dartplant.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DartPlantRuntimeKind {
    DARTPLANT_RUNTIME_FLUTTER_AOT = 0,
} DartPlantRuntimeKind;

typedef enum DartPlantArchitecture {
    DARTPLANT_ARCH_ARM64 = 0,
} DartPlantArchitecture;

enum {
    // Legacy/manual raw callback mapping flags. Normal consumers never need
    // these: verified per-Function DartCallLayout is built internally from
    // compiler evidence and the Dart calling convention.
    DARTPLANT_PROFILE_RAW_GP_ARGUMENTS = 1u << 0,
    DARTPLANT_PROFILE_RAW_GP_RESULT = 1u << 1,
    DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS = 1u << 2,
    DARTPLANT_PROFILE_TAGGED_GP_RESULT = 1u << 3,
};

typedef enum DartPlantAbiLocationKind {
    DARTPLANT_ABI_GP_REGISTER = 0,
    DARTPLANT_ABI_FP_REGISTER,
} DartPlantAbiLocationKind;

typedef struct DartPlantAbiLocation {
    DartPlantAbiLocationKind kind;
    uint8_t index;
    uint8_t reserved[2];
} DartPlantAbiLocation;

// Advanced explicit-runtime compatibility profile. Identity/module fields are
// still used by the advanced runtime API. argument_locations/result_location
// are only the legacy raw-mapping escape hatch and are not a typed ABI source.
typedef struct DartPlantRuntimeProfile {
    uint32_t struct_size;
    uint32_t profile_version;
    DartPlantRuntimeKind runtime_kind;
    DartPlantArchitecture architecture;
    uint32_t pointer_size;
    uint32_t flags;

    const char* profile_name;
    const char* dart_version;
    const char* flutter_version;
    const char* app_module_name;
    const char* app_build_id;
    const char* runtime_module_name;
    const char* runtime_build_id;

    uint32_t argument_count;
    DartPlantAbiLocation argument_locations[8];
    DartPlantAbiLocation result_location;
    uint8_t thread_gp_register;
    uint8_t pool_gp_register;
    uint8_t reserved[3];
} DartPlantRuntimeProfile;

// Initializes the conservative Flutter ARM64 identity/profile used by the
// advanced explicit-runtime API. Normal consumers use dartplant_init().
DARTPLANT_EXPORT void dartplant_runtime_profile_init_arm64_aot(DartPlantRuntimeProfile* profile);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_ADVANCED_RUNTIME_PROFILE_H_
