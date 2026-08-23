// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_INVOCATION_H_
#define DARTPLANT_INVOCATION_H_

#include <stdint.h>

#include "dartplant/dartplant.h"
#include "dartplant/vm_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DartPlantInvocation DartPlantInvocation;

typedef void (*DartPlantInvocationCallback)(DartPlantInvocation* invocation, void* user_data);

typedef enum DartPlantHookFlags {
    // A shared AOT Code entry can represent multiple logical Dart Functions.
    // Method callbacks fail closed for known shared targets unless the caller
    // explicitly opts into a physical CodeTarget hook with ambiguous identity.
    DARTPLANT_HOOK_ALLOW_SHARED_CODE = 1u << 0,
} DartPlantHookFlags;

typedef struct DartPlantHookOptions {
    uint32_t struct_size;
    uint32_t flags;
    DartPlantInvocationCallback on_enter;
    DartPlantInvocationCallback on_leave;
    void* user_data;
    DartPlantVmAdapter* vm_adapter;
} DartPlantHookOptions;

typedef enum DartPlantValueKind {
    DARTPLANT_VALUE_UNKNOWN = 0,
    DARTPLANT_VALUE_RAW_WORD,
    DARTPLANT_VALUE_NULL,
    DARTPLANT_VALUE_BOOL,
    DARTPLANT_VALUE_SMI,
    DARTPLANT_VALUE_DOUBLE,
    DARTPLANT_VALUE_HEAP_OBJECT,  // Opaque tagged heap reference for callback lifetime only.
} DartPlantValueKind;

typedef struct DartPlantValue {
    DartPlantValueKind kind;
    uint32_t reserved;
    uint64_t raw;  // Opaque raw bits: tagged word or IEEE-754 payload.
} DartPlantValue;

typedef struct DartPlantArm64Context {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint8_t v[32][16];
} DartPlantArm64Context;

typedef enum DartPlantInvocationPhase {
    DARTPLANT_INVOCATION_ENTER = 0,
    DARTPLANT_INVOCATION_LEAVE,
} DartPlantInvocationPhase;

typedef struct DartPlantMethodIdentityInfo {
    uint32_t struct_size;
    const char* library_uri;
    const char* class_name;
    const char* function_name;
    const char* signature;
    DartPlantEntryKind entry_kind;
} DartPlantMethodIdentityInfo;

// Compatibility alias for dartplant_invocation_requested_method(). On shared
// CodeTargets this is the method under which the current listener was
// registered, not proof of the logical caller that reached the physical entry.
DARTPLANT_EXPORT const DartPlantMethod* dartplant_invocation_method(
    const DartPlantInvocation* invocation);
DARTPLANT_EXPORT const DartPlantMethod* dartplant_invocation_requested_method(
    const DartPlantInvocation* invocation);
// Returns the logical method only when the physical CodeTarget has a unique
// identity. Shared CodeTargets return null because the entry alone cannot prove
// which alias caused this invocation.
DARTPLANT_EXPORT const DartPlantMethod* dartplant_invocation_logical_method(
    const DartPlantInvocation* invocation);
DARTPLANT_EXPORT uintptr_t
dartplant_invocation_code_target_address(const DartPlantInvocation* invocation);
DARTPLANT_EXPORT uint8_t
dartplant_invocation_identity_ambiguous(const DartPlantInvocation* invocation);
// Total aliases reported for this physical entry by the resolver/VM scan. This
// can be greater than the number of identities DartPlant has resolved so far.
DARTPLANT_EXPORT uint32_t
dartplant_invocation_code_alias_count(const DartPlantInvocation* invocation);
// Known alias identities are snapshotted at invocation entry. Returned string
// pointers remain valid only for the lifetime of the current callback.
DARTPLANT_EXPORT uint32_t
dartplant_invocation_known_code_alias_count(const DartPlantInvocation* invocation);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_code_alias(
    const DartPlantInvocation* invocation, uint32_t index, DartPlantMethodIdentityInfo* out_alias);
DARTPLANT_EXPORT DartPlantInvocationPhase
dartplant_invocation_phase(const DartPlantInvocation* invocation);
DARTPLANT_EXPORT uint32_t dartplant_invocation_depth(const DartPlantInvocation* invocation);

DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_gp_register(
    const DartPlantInvocation* invocation, uint32_t register_index, uint64_t* out_value);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_set_gp_register(
    DartPlantInvocation* invocation, uint32_t register_index, uint64_t value);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_fp_register(
    const DartPlantInvocation* invocation, uint32_t register_index, uint64_t* out_value);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_set_fp_register(
    DartPlantInvocation* invocation, uint32_t register_index, uint64_t value);

DARTPLANT_EXPORT uint32_t
dartplant_invocation_argument_count(const DartPlantInvocation* invocation);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_argument(
    const DartPlantInvocation* invocation, uint32_t index, DartPlantValue* out_value);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_set_argument(DartPlantInvocation* invocation,
                                                                   uint32_t index,
                                                                   const DartPlantValue* value);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_retain_argument_object(
    DartPlantInvocation* invocation, uint32_t index, DartPlantObjectStrength strength,
    DartPlantObjectHandle** out_handle);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_set_argument_object(
    DartPlantInvocation* invocation, uint32_t index, const DartPlantObjectHandle* handle);
DARTPLANT_EXPORT DartPlantStatus
dartplant_invocation_get_result(const DartPlantInvocation* invocation, DartPlantValue* out_value);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_set_result(DartPlantInvocation* invocation,
                                                                 const DartPlantValue* value);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_retain_result_object(
    DartPlantInvocation* invocation, DartPlantObjectStrength strength,
    DartPlantObjectHandle** out_handle);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_set_result_object(
    DartPlantInvocation* invocation, const DartPlantObjectHandle* handle);
DARTPLANT_EXPORT DartPlantStatus
dartplant_invocation_skip_original(DartPlantInvocation* invocation);
DARTPLANT_EXPORT DartPlantStatus
dartplant_invocation_call_original(DartPlantInvocation* invocation);
DARTPLANT_EXPORT uint8_t
dartplant_invocation_is_original_skipped(const DartPlantInvocation* invocation);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_INVOCATION_H_
