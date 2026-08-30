// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ADVANCED_DIAGNOSTICS_H_
#define DARTPLANT_ADVANCED_DIAGNOSTICS_H_

#include <stdint.h>

#include "dartplant/dartplant.h"

#ifdef __cplusplus
extern "C" {
#endif

// Structured resolver stages are deliberately stable and orthogonal to the
// final DartPlantStatus. Hosts can report where proof stopped without parsing
// English error strings.
typedef enum DartPlantResolveStage {
    DARTPLANT_RESOLVE_NOT_STARTED = 0,
    DARTPLANT_RESOLVE_MODULE_SELECTION,
    DARTPLANT_RESOLVE_SNAPSHOT_IDENTITY,
    DARTPLANT_RESOLVE_VM_PROFILE,
    DARTPLANT_RESOLVE_LIVE_VM,
    DARTPLANT_RESOLVE_FUNCTION_IDENTITY,
    DARTPLANT_RESOLVE_CODE_TARGET,
    DARTPLANT_RESOLVE_ARTIFACT_BINDING,
    DARTPLANT_RESOLVE_ABI_EVIDENCE,
    DARTPLANT_RESOLVE_STRUCTURAL_EVIDENCE,
    DARTPLANT_RESOLVE_ENTRY_KIND,
    DARTPLANT_RESOLVE_HOOK_INSTALL,
    DARTPLANT_RESOLVE_COMPLETE,
} DartPlantResolveStage;

typedef enum DartPlantResolveOutcome {
    DARTPLANT_RESOLVE_IN_PROGRESS = 0,
    DARTPLANT_RESOLVE_RESOLVED,
    DARTPLANT_RESOLVE_REJECTED,
} DartPlantResolveOutcome;

typedef enum DartPlantResolveRejectReason {
    DARTPLANT_REJECT_NONE = 0,
    DARTPLANT_REJECT_INVALID_ARGUMENT,
    DARTPLANT_REJECT_MODULE_NOT_FOUND,
    DARTPLANT_REJECT_MODULE_AMBIGUOUS,
    DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE,
    DARTPLANT_REJECT_SNAPSHOT_MISMATCH,
    DARTPLANT_REJECT_PROFILE_UNSUPPORTED,
    DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE,
    DARTPLANT_REJECT_STALE_GENERATION,
    DARTPLANT_REJECT_FUNCTION_NOT_FOUND,
    DARTPLANT_REJECT_FUNCTION_AMBIGUOUS,
    DARTPLANT_REJECT_CODE_TARGET_AMBIGUOUS,
    DARTPLANT_REJECT_ARTIFACT_MISMATCH,
    DARTPLANT_REJECT_ABI_INCOMPLETE,
    DARTPLANT_REJECT_ABI_CONFLICT,
    DARTPLANT_REJECT_STRUCTURAL_INCOMPLETE,
    DARTPLANT_REJECT_STRUCTURAL_CONFLICT,
    DARTPLANT_REJECT_ENTRY_KIND_UNSUPPORTED,
    DARTPLANT_REJECT_HOOK_FAILED,
} DartPlantResolveRejectReason;

typedef struct DartPlantResolutionDiagnostics {
    uint32_t struct_size;
    DartPlantResolveStage stage;
    DartPlantResolveOutcome outcome;
    DartPlantResolveRejectReason reject_reason;
    DartPlantStatus status;

    uint64_t runtime_generation;
    DartPlantEntryKind requested_entry_kind;
    uint32_t module_candidate_count;
    uint32_t function_candidate_count;
    uint32_t code_alias_count;
    uint32_t abi_provider_count;
    uint32_t structural_candidate_count;
    uint32_t structural_relation_count;
    uint64_t selected_entry;
} DartPlantResolutionDiagnostics;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_ADVANCED_DIAGNOSTICS_H_
