// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ABI_EVIDENCE_H_
#define DARTPLANT_ABI_EVIDENCE_H_

#include <stdint.h>

#include "dartplant/dartplant.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DartPlantAbiRepresentation {
    DARTPLANT_ABI_REPRESENTATION_UNKNOWN = 0,
    DARTPLANT_ABI_REPRESENTATION_TAGGED,
    DARTPLANT_ABI_REPRESENTATION_UNBOXED_INT64,
    DARTPLANT_ABI_REPRESENTATION_UNBOXED_DOUBLE,
    DARTPLANT_ABI_REPRESENTATION_PAIR_OF_TAGGED,
} DartPlantAbiRepresentation;

// Exact per-Function ABI facts exported by the same compiler pipeline that
// produced the target AOT image. This is a tooling/sidecar ingestion surface;
// ordinary DartPlant consumers should not need to construct it manually.
//
// parameter_representations covers FunctionType formal slots in entry order,
// including VM implicit formals such as an instance receiver. Optional
// transport is recorded explicitly and currently keeps typed call-layout
// construction fail-closed.
typedef struct DartPlantCompilerAbiEvidence {
    uint32_t struct_size;
    // Artifact binding. These three fields are required: ABI facts must be
    // tied to the exact AOT image incarnation and physical code bytes instead
    // of being accepted only because a logical method name happened to match.
    const char* snapshot_hash;
    const char* app_build_id;
    const char* code_fingerprint;
    const DartPlantAbiRepresentation* parameter_representations;
    uint32_t parameter_count;
    DartPlantAbiRepresentation result_representation;
    uint32_t max_parameters_in_registers;
    uint8_t must_use_stack_calling_convention;
    uint8_t has_optional_parameters;
    uint8_t has_overrides_with_less_direct_parameters;
    uint8_t reserved;
} DartPlantCompilerAbiEvidence;

typedef enum DartPlantMethodAbiState {
    DARTPLANT_METHOD_ABI_NONE = 0,
    DARTPLANT_METHOD_ABI_INCOMPLETE,
    DARTPLANT_METHOD_ABI_VERIFIED,
    DARTPLANT_METHOD_ABI_CONFLICTING,
    DARTPLANT_METHOD_ABI_UNSUPPORTED,
} DartPlantMethodAbiState;

typedef struct DartPlantMethodAbiInfo {
    uint32_t struct_size;
    DartPlantMethodAbiState state;
    uint32_t parameter_count;
    uint32_t stack_words;
    uint8_t has_verified_call_layout;
    uint8_t reserved[3];
} DartPlantMethodAbiInfo;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_ABI_EVIDENCE_H_
