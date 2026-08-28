// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_RUNTIME_PROFILE_H_
#define DARTPLANT_RUNTIME_PROFILE_H_

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
    // The profile explicitly maps logical arguments to ARM64 GP registers.
    DARTPLANT_PROFILE_RAW_GP_ARGUMENTS = 1u << 0,
    // The profile explicitly maps the raw return word to an ARM64 GP register.
    DARTPLANT_PROFILE_RAW_GP_RESULT = 1u << 1,
    // All mapped GP arguments carry Dart tagged values. This proof enables
    // Smi, heap-object, and canonical-null semantic decoding/encoding.
    DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS = 1u << 2,
    // The mapped GP result carries a Dart tagged value.
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

    // Legacy/manual callback mapping. These fields are an explicit raw escape
    // hatch and compatibility surface, not the source of truth for future
    // typed Dart invocation. Verified per-Function DartCallLayout is computed
    // internally from exact ABI evidence + the Dart calling convention.
    uint32_t argument_count;
    DartPlantAbiLocation argument_locations[8];
    DartPlantAbiLocation result_location;
    uint8_t thread_gp_register;
    uint8_t pool_gp_register;
    uint8_t reserved[3];
} DartPlantRuntimeProfile;

// Dart's ARM64 AOT register convention uses GP argument registers
// x1, x2, x3, x5, x6, x7 and FP argument registers v0-v5. x16/x17 are veneer scratch registers
// and x30 is the continuation link. The result location is profile-specific.

// Initializes a conservative ARM64 AOT profile. It identifies the modules and
// version but does not enable argument/result decoding. A project-specific
// profile must opt in after validating the target Dart ABI.
DARTPLANT_EXPORT void dartplant_runtime_profile_init_arm64_aot(DartPlantRuntimeProfile* profile);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_RUNTIME_PROFILE_H_
