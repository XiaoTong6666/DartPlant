// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstring>

#include "runtime/dart_vm_abi.h"
#include "runtime/runtime_internal.h"

namespace {

bool ValidRegister(uint32_t index) { return index < 31; }

bool ArgumentAccessEnabled(const DartPlantInvocation* invocation) {
    return invocation != nullptr && invocation->profile != nullptr &&
           (invocation->profile->flags & DARTPLANT_PROFILE_RAW_GP_ARGUMENTS) != 0;
}

bool ResultAccessEnabled(const DartPlantInvocation* invocation) {
    return invocation != nullptr && invocation->profile != nullptr &&
           (invocation->profile->flags & DARTPLANT_PROFILE_RAW_GP_RESULT) != 0;
}

bool TaggedArgumentAccessEnabled(const DartPlantInvocation* invocation) {
    return ArgumentAccessEnabled(invocation) &&
           (invocation->profile->flags & DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS) != 0;
}

bool TaggedResultAccessEnabled(const DartPlantInvocation* invocation) {
    return ResultAccessEnabled(invocation) &&
           (invocation->profile->flags & DARTPLANT_PROFILE_TAGGED_GP_RESULT) != 0;
}

uint64_t ActiveValidatedNullValue(const DartPlantInvocation* invocation) {
    if (invocation == nullptr || invocation->hook == nullptr ||
        invocation->hook->runtime_generation == nullptr) {
        return invocation == nullptr ? 0 : invocation->validated_null_value;
    }
    return invocation->hook->runtime_generation->load(std::memory_order_acquire) ==
                   invocation->hook->expected_runtime_generation
               ? invocation->validated_null_value
               : 0;
}

bool ReadLocation(const DartPlantInvocation* invocation, const DartPlantAbiLocation& location,
                  uint64_t* out_value) {
    if (location.kind == DARTPLANT_ABI_FP_REGISTER) {
        if (location.index >= 32) return false;
        std::memcpy(out_value, invocation->context->v[location.index], sizeof(*out_value));
        return true;
    }
    if (!ValidRegister(location.index)) return false;
    *out_value = invocation->context->x[location.index];
    return true;
}

bool WriteLocation(DartPlantInvocation* invocation, const DartPlantAbiLocation& location,
                   uint64_t value) {
    if (location.kind == DARTPLANT_ABI_FP_REGISTER) {
        if (location.index >= 32) return false;
        std::memcpy(invocation->context->v[location.index], &value, sizeof(value));
        return true;
    }
    if (!ValidRegister(location.index)) return false;
    invocation->context->x[location.index] = value;
    return true;
}

}  // namespace

extern "C" {

const DartPlantMethod* dartplant_invocation_method(const DartPlantInvocation* invocation) {
    return dartplant_invocation_requested_method(invocation);
}

const DartPlantMethod* dartplant_invocation_requested_method(
    const DartPlantInvocation* invocation) {
    return invocation == nullptr ? nullptr : invocation->requested_method;
}

const DartPlantMethod* dartplant_invocation_logical_method(const DartPlantInvocation* invocation) {
    return invocation == nullptr || invocation->identity_ambiguous ? nullptr
                                                                   : invocation->requested_method;
}

uintptr_t dartplant_invocation_code_target_address(const DartPlantInvocation* invocation) {
    return invocation == nullptr || invocation->code_target == nullptr
               ? 0
               : invocation->code_target->entry;
}

uint8_t dartplant_invocation_identity_ambiguous(const DartPlantInvocation* invocation) {
    return invocation != nullptr && invocation->identity_ambiguous ? 1 : 0;
}

uint32_t dartplant_invocation_code_alias_count(const DartPlantInvocation* invocation) {
    return invocation == nullptr || invocation->code_target == nullptr
               ? 0
               : invocation->code_target->AliasCount();
}

uint32_t dartplant_invocation_known_code_alias_count(const DartPlantInvocation* invocation) {
    return invocation == nullptr ? 0
                                 : static_cast<uint32_t>(invocation->code_alias_snapshot.size());
}

DartPlantStatus dartplant_invocation_get_code_alias(const DartPlantInvocation* invocation,
                                                    uint32_t index,
                                                    DartPlantMethodIdentityInfo* out_alias) {
    if (invocation == nullptr || out_alias == nullptr ||
        out_alias->struct_size < sizeof(DartPlantMethodIdentityInfo) ||
        index >= invocation->code_alias_snapshot.size()) {
        dartplant::SetLastError("code alias query arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const auto& alias = invocation->code_alias_snapshot[index];
    out_alias->library_uri = alias.library_uri.c_str();
    out_alias->class_name = alias.class_name.c_str();
    out_alias->function_name = alias.function_name.c_str();
    out_alias->signature = alias.signature.c_str();
    out_alias->entry_kind = alias.entry_kind;
    return DARTPLANT_OK;
}

DartPlantInvocationPhase dartplant_invocation_phase(const DartPlantInvocation* invocation) {
    return invocation == nullptr ? DARTPLANT_INVOCATION_ENTER : invocation->phase;
}

uint32_t dartplant_invocation_depth(const DartPlantInvocation* invocation) {
    return invocation == nullptr ? 0 : invocation->depth;
}

DartPlantStatus dartplant_invocation_get_gp_register(const DartPlantInvocation* invocation,
                                                     uint32_t register_index, uint64_t* out_value) {
    if (invocation == nullptr || invocation->context == nullptr || out_value == nullptr ||
        !ValidRegister(register_index)) {
        dartplant::SetLastError("GP register read arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    *out_value = invocation->context->x[register_index];
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_get_fp_register(const DartPlantInvocation* invocation,
                                                     uint32_t register_index, uint64_t* out_value) {
    if (invocation == nullptr || invocation->context == nullptr || out_value == nullptr ||
        register_index >= 32) {
        dartplant::SetLastError("FP register read arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::memcpy(out_value, invocation->context->v[register_index], sizeof(*out_value));
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_set_fp_register(DartPlantInvocation* invocation,
                                                     uint32_t register_index, uint64_t value) {
    if (invocation == nullptr || invocation->context == nullptr || register_index >= 32) {
        dartplant::SetLastError("FP register write arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::memcpy(invocation->context->v[register_index], &value, sizeof(value));
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_set_gp_register(DartPlantInvocation* invocation,
                                                     uint32_t register_index, uint64_t value) {
    if (invocation == nullptr || invocation->context == nullptr || !ValidRegister(register_index)) {
        dartplant::SetLastError("GP register write arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    invocation->context->x[register_index] = value;
    return DARTPLANT_OK;
}

uint32_t dartplant_invocation_argument_count(const DartPlantInvocation* invocation) {
    return ArgumentAccessEnabled(invocation) ? invocation->profile->argument_count : 0;
}

DartPlantStatus dartplant_invocation_get_argument(const DartPlantInvocation* invocation,
                                                  uint32_t index, DartPlantValue* out_value) {
    if (!ArgumentAccessEnabled(invocation) || invocation->context == nullptr ||
        out_value == nullptr || index >= invocation->profile->argument_count) {
        dartplant::SetLastError("argument decoding is not enabled by the profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    const auto& location = invocation->profile->argument_locations[index];
    uint64_t raw = 0;
    if (!ReadLocation(invocation, location, &raw)) {
        dartplant::SetLastError("profile argument register is invalid");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    *out_value = location.kind == DARTPLANT_ABI_FP_REGISTER
                     ? dartplant::dartplant_vm_abi_decode_fp_word(raw)
                     : dartplant::dartplant_vm_abi_decode_gp_word(
                           raw, TaggedArgumentAccessEnabled(invocation),
                           ActiveValidatedNullValue(invocation));
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_set_argument(DartPlantInvocation* invocation, uint32_t index,
                                                  const DartPlantValue* value) {
    if (!ArgumentAccessEnabled(invocation) || invocation->context == nullptr || value == nullptr ||
        index >= invocation->profile->argument_count) {
        dartplant::SetLastError("argument encoding is not enabled by the profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_ENTER) {
        dartplant::SetLastError("arguments can only be changed during enter");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    const auto& location = invocation->profile->argument_locations[index];
    uint64_t raw = 0;
    const DartPlantStatus status = location.kind == DARTPLANT_ABI_FP_REGISTER
                                       ? dartplant::dartplant_vm_abi_encode_fp_word(value, &raw)
                                       : dartplant::dartplant_vm_abi_encode_gp_word(
                                             value, TaggedArgumentAccessEnabled(invocation),
                                             ActiveValidatedNullValue(invocation), &raw);
    if (status != DARTPLANT_OK) {
        return status;
    }
    if (!WriteLocation(invocation, location, raw)) {
        return DARTPLANT_PROFILE_MISMATCH;
    }
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_retain_argument_object(DartPlantInvocation* invocation,
                                                            uint32_t index,
                                                            DartPlantObjectStrength strength,
                                                            DartPlantObjectHandle** out_handle) {
    if (!TaggedArgumentAccessEnabled(invocation) || invocation->context == nullptr ||
        index >= invocation->profile->argument_count || out_handle == nullptr) {
        dartplant::SetLastError("object argument retention is not enabled by the profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (invocation->vm_adapter == nullptr) {
        dartplant::SetLastError("invocation has no VM object adapter");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (strength != DARTPLANT_OBJECT_STRONG && strength != DARTPLANT_OBJECT_WEAK) {
        dartplant::SetLastError("object strength is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const auto& location = invocation->profile->argument_locations[index];
    if (location.kind == DARTPLANT_ABI_FP_REGISTER) {
        dartplant::SetLastError("FP arguments cannot be retained as VM objects");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    uint64_t raw = 0;
    if (!ReadLocation(invocation, location, &raw)) {
        dartplant::SetLastError("profile argument register is invalid");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    return dartplant::VmAdapterRetainObject(invocation->vm_adapter, raw, strength, out_handle);
}

DartPlantStatus dartplant_invocation_set_argument_object(DartPlantInvocation* invocation,
                                                         uint32_t index,
                                                         const DartPlantObjectHandle* handle) {
    if (!TaggedArgumentAccessEnabled(invocation) || invocation->context == nullptr ||
        handle == nullptr || index >= invocation->profile->argument_count) {
        dartplant::SetLastError("object argument write arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_ENTER) {
        dartplant::SetLastError("arguments can only be changed during enter");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    if (invocation->vm_adapter == nullptr) {
        dartplant::SetLastError("invocation has no VM object adapter");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const auto& location = invocation->profile->argument_locations[index];
    if (location.kind == DARTPLANT_ABI_FP_REGISTER) {
        dartplant::SetLastError("FP arguments cannot be set from VM objects");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const DartPlantStatus valid = dartplant::VmAdapterCheckHandle(invocation->vm_adapter, handle);
    if (valid != DARTPLANT_OK) return valid;
    uint64_t raw = 0;
    const DartPlantStatus status = dartplant::VmAdapterSetRaw(handle, &raw);
    if (status != DARTPLANT_OK || !WriteLocation(invocation, location, raw)) {
        return status == DARTPLANT_OK ? DARTPLANT_PROFILE_MISMATCH : status;
    }
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_get_result(const DartPlantInvocation* invocation,
                                                DartPlantValue* out_value) {
    if (!ResultAccessEnabled(invocation) || invocation->context == nullptr ||
        out_value == nullptr) {
        dartplant::SetLastError("result decoding is not enabled by the profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_LEAVE && !invocation->skip_original) {
        dartplant::SetLastError("result is only available during leave or after skipping original");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    uint64_t raw = 0;
    if (!ReadLocation(invocation, invocation->profile->result_location, &raw)) {
        return DARTPLANT_PROFILE_MISMATCH;
    }
    *out_value =
        invocation->profile->result_location.kind == DARTPLANT_ABI_FP_REGISTER
            ? dartplant::dartplant_vm_abi_decode_fp_word(raw)
            : dartplant::dartplant_vm_abi_decode_gp_word(raw, TaggedResultAccessEnabled(invocation),
                                                         ActiveValidatedNullValue(invocation));
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_set_result(DartPlantInvocation* invocation,
                                                const DartPlantValue* value) {
    if (!ResultAccessEnabled(invocation) || invocation->context == nullptr || value == nullptr) {
        dartplant::SetLastError("result encoding is not enabled by the profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    uint64_t raw = 0;
    const DartPlantStatus status =
        invocation->profile->result_location.kind == DARTPLANT_ABI_FP_REGISTER
            ? dartplant::dartplant_vm_abi_encode_fp_word(value, &raw)
            : dartplant::dartplant_vm_abi_encode_gp_word(
                  value, TaggedResultAccessEnabled(invocation),
                  ActiveValidatedNullValue(invocation), &raw);
    if (status != DARTPLANT_OK) {
        return status;
    }
    if (!WriteLocation(invocation, invocation->profile->result_location, raw)) {
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (invocation->phase == DARTPLANT_INVOCATION_ENTER) {
        invocation->skip_original = true;
        invocation->call_original = false;
    } else if (invocation->phase != DARTPLANT_INVOCATION_LEAVE) {
        dartplant::SetLastError("result cannot be changed in this phase");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_retain_result_object(DartPlantInvocation* invocation,
                                                          DartPlantObjectStrength strength,
                                                          DartPlantObjectHandle** out_handle) {
    if (!TaggedResultAccessEnabled(invocation) || invocation->context == nullptr ||
        out_handle == nullptr) {
        dartplant::SetLastError("object result retention is not enabled by the profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (invocation->vm_adapter == nullptr) {
        dartplant::SetLastError("invocation has no VM object adapter");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_LEAVE && !invocation->skip_original) {
        dartplant::SetLastError(
            "result object is only available during leave or after skipping original");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    if (strength != DARTPLANT_OBJECT_STRONG && strength != DARTPLANT_OBJECT_WEAK) {
        dartplant::SetLastError("object strength is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const auto& location = invocation->profile->result_location;
    if (location.kind == DARTPLANT_ABI_FP_REGISTER) {
        dartplant::SetLastError("FP results cannot be retained as VM objects");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    uint64_t raw = 0;
    if (!ReadLocation(invocation, location, &raw)) return DARTPLANT_PROFILE_MISMATCH;
    return dartplant::VmAdapterRetainObject(invocation->vm_adapter, raw, strength, out_handle);
}

DartPlantStatus dartplant_invocation_set_result_object(DartPlantInvocation* invocation,
                                                       const DartPlantObjectHandle* handle) {
    if (!TaggedResultAccessEnabled(invocation) || invocation->context == nullptr ||
        handle == nullptr) {
        dartplant::SetLastError("object result write arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_ENTER &&
        invocation->phase != DARTPLANT_INVOCATION_LEAVE) {
        dartplant::SetLastError("result cannot be changed in this phase");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    if (invocation->vm_adapter == nullptr) {
        dartplant::SetLastError("invocation has no VM object adapter");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const auto& location = invocation->profile->result_location;
    if (location.kind == DARTPLANT_ABI_FP_REGISTER) {
        dartplant::SetLastError("FP results cannot be set from VM objects");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const DartPlantStatus valid = dartplant::VmAdapterCheckHandle(invocation->vm_adapter, handle);
    if (valid != DARTPLANT_OK) return valid;
    uint64_t raw = 0;
    const DartPlantStatus status = dartplant::VmAdapterSetRaw(handle, &raw);
    if (status != DARTPLANT_OK || !WriteLocation(invocation, location, raw)) {
        return status == DARTPLANT_OK ? DARTPLANT_PROFILE_MISMATCH : status;
    }
    if (invocation->phase == DARTPLANT_INVOCATION_ENTER) {
        invocation->skip_original = true;
        invocation->call_original = false;
    }
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_skip_original(DartPlantInvocation* invocation) {
    if (invocation == nullptr) {
        dartplant::SetLastError("invocation is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_ENTER) {
        dartplant::SetLastError("original can only be skipped during enter");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    invocation->skip_original = true;
    invocation->call_original = false;
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_call_original(DartPlantInvocation* invocation) {
    if (invocation == nullptr) {
        dartplant::SetLastError("invocation is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_ENTER) {
        dartplant::SetLastError("original can only be called during enter");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    if (invocation->original_called) {
        dartplant::SetLastError("original has already been invoked");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    if (invocation->hook == nullptr || invocation->hook->backup == nullptr) {
        dartplant::SetLastError("original entry is unavailable");
        return DARTPLANT_HOOK_FAILED;
    }
    dartplant_arm64_invoke_original(invocation->context, invocation->hook->backup);
    invocation->original_called = true;
    invocation->skip_original = false;
    invocation->call_original = true;
    invocation->skip_original = true;
    return DARTPLANT_OK;
}

uint8_t dartplant_invocation_is_original_skipped(const DartPlantInvocation* invocation) {
    return invocation != nullptr && invocation->skip_original ? 1 : 0;
}

}  // extern "C"
