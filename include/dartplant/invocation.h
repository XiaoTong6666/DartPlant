// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_INVOCATION_H_
#define DARTPLANT_INVOCATION_H_

#include <stdint.h>

#include "dartplant/dartplant.h"
#include "dartplant/hook.h"
#include "dartplant/vm_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum DartPlantValueKind {
    DARTPLANT_VALUE_UNKNOWN = 0,
    DARTPLANT_VALUE_RAW_WORD,
    DARTPLANT_VALUE_NULL,
    DARTPLANT_VALUE_BOOL,
    DARTPLANT_VALUE_SMI,
    DARTPLANT_VALUE_DOUBLE,
    DARTPLANT_VALUE_HEAP_OBJECT,  // Opaque tagged heap reference for callback lifetime only.
    DARTPLANT_VALUE_INT64,
} DartPlantValueKind;

typedef struct DartPlantValue {
    DartPlantValueKind kind;
    uint32_t reserved;
    // Kind-dependent payload. RAW_WORD/SMI/HEAP_OBJECT keep the VM word,
    // INT64 keeps the two's-complement integer bits, DOUBLE keeps IEEE-754
    // bits, and BOOL uses the semantic value 0 or 1.
    uint64_t raw;
} DartPlantValue;

typedef struct DartPlantValuePair {
    DartPlantValue first;
    DartPlantValue second;
} DartPlantValuePair;

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
    DARTPLANT_INVOCATION_EXCEPTION,
} DartPlantInvocationPhase;

typedef struct DartPlantMethodIdentityInfo {
    uint32_t struct_size;
    const char* library_uri;
    const char* class_name;
    const char* function_name;
    const char* signature;
    DartPlantEntryKind entry_kind;
} DartPlantMethodIdentityInfo;

// Compatibility alias for dartplant_invocation_requested_method(). Shared
// entry targets use the method under which the current listener was
// registered, not proof of the logical caller that reached the physical entry.
DARTPLANT_EXPORT const DartPlantMethod* dartplant_invocation_method(
    const DartPlantInvocation* invocation);
DARTPLANT_EXPORT const DartPlantMethod* dartplant_invocation_requested_method(
    const DartPlantInvocation* invocation);
// Returns the logical method only when the physical entry target has a unique
// identity. Shared entry targets return null because the entry alone cannot prove
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

// True only when this physical invocation has a verified per-Function
// DartCallLayout. This proves argument/result transport, not canonical Dart
// semantic roots. An artifact-first hook installed before Live VM bootstrap can
// therefore have verified ABI while NULL/BOOL refinement remains unavailable:
// tagged values still decode safely as SMI/HEAP_OBJECT, and writing NULL/BOOL
// fails closed unless canonical roots were already validated when that physical
// hook was installed. A later bootstrap does not retroactively upgrade an
// existing hook. Raw GP/FP context access remains available when this is false.
DARTPLANT_EXPORT uint8_t
dartplant_invocation_has_verified_abi(const DartPlantInvocation* invocation);

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
// PRODUCT ARM64 AOT closure calls carry the Closure object itself in x0. It is
// a hidden receiver and is deliberately not counted as a FunctionType formal.
// These helpers are available only when DartPlant has exact default-entry
// closure-call evidence (live VM or compiler artifact + SDK contract); a typed
// DartCallLayout is not required. Access is enter-only, before x0 is repurposed
// as the result register. With an exact VM V3 adapter, the receiver may also be
// retained through the adapter's generated-root lease.
DARTPLANT_EXPORT uint8_t
dartplant_invocation_has_closure_receiver(const DartPlantInvocation* invocation);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_closure_receiver(
    const DartPlantInvocation* invocation, DartPlantValue* out_value);
typedef struct DartPlantArgumentsDescriptorInfo {
    uint32_t struct_size;
    uint32_t type_args_len;
    uint32_t count;
    uint32_t size;
    uint32_t positional_count;
    uint32_t named_count;
    uint64_t raw_descriptor;
} DartPlantArgumentsDescriptorInfo;

// Reads the verified PRODUCT ARM64 closure ArgumentsDescriptor carried in x4.
// The descriptor is an immutable VM Array of Smis. This API exposes only its
// raw VM call-shape counters. For closure calls, count/size/positional_count
// include the hidden Closure receiver as one boxed argument, while
// dartplant_invocation_argument_count() and get_argument() expose only the
// FunctionType user formals. Named argument strings/positions are mapped only
// when the retained FunctionType and descriptor provide an exact match.
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_arguments_descriptor(
    const DartPlantInvocation* invocation, DartPlantArgumentsDescriptorInfo* out_info);
// Returns the explicitly passed generic closure TypeArguments vector as one
// opaque tagged object. Enter phase only. The vector itself is never exposed
// for arbitrary VM-memory access or mutation.
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_closure_type_arguments(
    const DartPlantInvocation* invocation, DartPlantValue* out_value);
// Reads one generic closure TypeArguments element that was captured while the
// mutator was still in generated state and immediately added to the callback's
// VM-visible generated-root lease. The callback therefore observes the
// relocated root after moving GC rather than dereferencing the original vector.
// Exact VM V3 support is required; element mutation/construction is not exposed.
// Enter phase only.
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_closure_type_argument(
    const DartPlantInvocation* invocation, uint32_t index, DartPlantValue* out_value);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_retain_closure_type_arguments(
    DartPlantInvocation* invocation, DartPlantObjectHandle** out_handle);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_exception(
    const DartPlantInvocation* invocation, DartPlantValue* out_value);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_stacktrace(
    const DartPlantInvocation* invocation, DartPlantValue* out_value);
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
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_get_result_pair(
    const DartPlantInvocation* invocation, DartPlantValuePair* out_value);
DARTPLANT_EXPORT DartPlantStatus dartplant_invocation_set_result_pair(
    DartPlantInvocation* invocation, const DartPlantValuePair* value);
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
