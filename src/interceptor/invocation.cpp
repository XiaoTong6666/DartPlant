// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "abi/value_codec.h"
#include "runtime/runtime_internal.h"
#include "vm/runtime_profiles.h"

namespace {

bool ValidRegister(uint32_t index) { return index < 31; }

bool HasVerifiedCallLayout(const DartPlantInvocation* invocation) {
    return invocation != nullptr && invocation->call_layout != nullptr;
}

template <typename T>
bool ReadSelfValue(uintptr_t address, T* out_value) {
    if (address == 0 || out_value == nullptr) return false;
    iovec local{out_value, sizeof(T)};
    iovec remote{reinterpret_cast<void*>(address), sizeof(T)};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(sizeof(T));
}

bool ReadPositiveCompressedSmi(uintptr_t address, const dartplant::RawObjectLayout& layout,
                               uint32_t* out_value) {
    if (out_value == nullptr || layout.compressed_word_size == 0 || layout.smi_tag_shift >= 64)
        return false;
    uint64_t raw = 0;
    if (layout.compressed_word_size == sizeof(uint32_t)) {
        uint32_t compressed = 0;
        if (!ReadSelfValue(address, &compressed)) return false;
        raw = compressed;
    } else if (layout.compressed_word_size == sizeof(uint64_t)) {
        if (!ReadSelfValue(address, &raw)) return false;
    } else {
        return false;
    }
    if ((raw & layout.smi_tag_mask) != layout.smi_tag) return false;
    const uint64_t value = raw >> layout.smi_tag_shift;
    if (value > UINT32_MAX) return false;
    *out_value = static_cast<uint32_t>(value);
    return true;
}

bool ReadVerifiedLocation(const DartPlantInvocation* invocation,
                          const dartplant::abi::DartAbiLocation& location, uint64_t* out_value);

bool ArgumentAccessEnabled(const DartPlantInvocation* invocation) {
    return HasVerifiedCallLayout(invocation) ||
           (invocation != nullptr && invocation->profile != nullptr &&
            (invocation->profile->flags & DARTPLANT_PROFILE_RAW_GP_ARGUMENTS) != 0);
}

bool ResultAccessEnabled(const DartPlantInvocation* invocation) {
    return HasVerifiedCallLayout(invocation) ||
           (invocation != nullptr && invocation->profile != nullptr &&
            (invocation->profile->flags & DARTPLANT_PROFILE_RAW_GP_RESULT) != 0);
}

bool ClosureReceiverAccessEnabled(const DartPlantInvocation* invocation) {
    if (invocation == nullptr) return false;
    if (invocation->closure_receiver_in_x0) return true;
    return HasVerifiedCallLayout(invocation) && invocation->call_layout->has_closure_receiver &&
           invocation->call_layout->closure_receiver_location.kind ==
               dartplant::abi::DartAbiLocationKind::kGpRegister;
}

bool ReadClosureReceiverRaw(const DartPlantInvocation* invocation, uint64_t* out_raw) {
    if (invocation == nullptr || invocation->context == nullptr || out_raw == nullptr) return false;
    if (invocation->closure_receiver_in_x0) {
        *out_raw = invocation->context->x[0];
        return true;
    }
    return HasVerifiedCallLayout(invocation) && invocation->call_layout->has_closure_receiver &&
           ReadVerifiedLocation(invocation, invocation->call_layout->closure_receiver_location,
                                out_raw);
}

bool LegacyTaggedArgumentAccessEnabled(const DartPlantInvocation* invocation) {
    return invocation != nullptr && invocation->profile != nullptr &&
           ArgumentAccessEnabled(invocation) &&
           (invocation->profile->flags & DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS) != 0;
}

bool LegacyTaggedResultAccessEnabled(const DartPlantInvocation* invocation) {
    return invocation != nullptr && invocation->profile != nullptr &&
           ResultAccessEnabled(invocation) &&
           (invocation->profile->flags & DARTPLANT_PROFILE_TAGGED_GP_RESULT) != 0;
}

bool ArgumentIsTagged(const DartPlantInvocation* invocation, uint32_t index) {
    if (HasVerifiedCallLayout(invocation)) {
        return index < invocation->call_layout->parameters.size() &&
               invocation->call_layout->parameters[index].representation ==
                   dartplant::abi::DartAbiRepresentation::kTagged;
    }
    return LegacyTaggedArgumentAccessEnabled(invocation);
}

bool ResultIsTagged(const DartPlantInvocation* invocation) {
    if (HasVerifiedCallLayout(invocation)) {
        return invocation->call_layout->result.representation ==
               dartplant::abi::DartAbiRepresentation::kTagged;
    }
    return LegacyTaggedResultAccessEnabled(invocation);
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

bool ActiveValidatedBoolValues(const DartPlantInvocation* invocation, uint64_t* out_true,
                               uint64_t* out_false) {
    if (invocation == nullptr || out_true == nullptr || out_false == nullptr) return false;
    if (invocation->hook != nullptr && invocation->hook->runtime_generation != nullptr &&
        invocation->hook->runtime_generation->load(std::memory_order_acquire) !=
            invocation->hook->expected_runtime_generation) {
        return false;
    }
    const uint64_t bool_true = invocation->validated_bool_true_value;
    const uint64_t bool_false = invocation->validated_bool_false_value;
    if (bool_true == 0 || bool_false == 0 || bool_true == bool_false) return false;
    *out_true = bool_true;
    *out_false = bool_false;
    return true;
}

DartPlantValue RefineTaggedSemanticValue(const DartPlantInvocation* invocation,
                                         DartPlantValue value) {
    if (value.kind != DARTPLANT_VALUE_HEAP_OBJECT) return value;
    uint64_t bool_true = 0;
    uint64_t bool_false = 0;
    if (!ActiveValidatedBoolValues(invocation, &bool_true, &bool_false)) return value;
    if (value.raw == bool_true) return {DARTPLANT_VALUE_BOOL, 0, 1};
    if (value.raw == bool_false) return {DARTPLANT_VALUE_BOOL, 0, 0};
    return value;
}

DartPlantStatus EncodeGpSemanticValue(const DartPlantInvocation* invocation,
                                      const DartPlantValue* value, bool is_tagged,
                                      uint64_t* out_raw) {
    if (value == nullptr || out_raw == nullptr) {
        dartplant::SetLastError("GP semantic value encoding arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (value->kind != DARTPLANT_VALUE_BOOL) {
        return dartplant::dartplant_vm_abi_encode_gp_word(
            value, is_tagged, ActiveValidatedNullValue(invocation), out_raw);
    }
    if (!is_tagged) {
        dartplant::SetLastError("Bool value encoding requires a tagged GP location");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (value->raw > 1) {
        dartplant::SetLastError("Bool semantic values must use raw=0 or raw=1");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    uint64_t bool_true = 0;
    uint64_t bool_false = 0;
    if (!ActiveValidatedBoolValues(invocation, &bool_true, &bool_false)) {
        dartplant::SetLastError("Bool value encoding requires validated canonical Bool roots");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    const DartPlantValue heap_value = {
        DARTPLANT_VALUE_HEAP_OBJECT,
        0,
        value->raw != 0 ? bool_true : bool_false,
    };
    return dartplant::dartplant_vm_abi_encode_gp_word(
        &heap_value, true, ActiveValidatedNullValue(invocation), out_raw);
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

bool ReadVerifiedLocation(const DartPlantInvocation* invocation,
                          const dartplant::abi::DartAbiLocation& location, uint64_t* out_value) {
    if (invocation == nullptr || invocation->context == nullptr || out_value == nullptr)
        return false;
    switch (location.kind) {
    case dartplant::abi::DartAbiLocationKind::kGpRegister:
        if (!ValidRegister(location.register_index)) return false;
        *out_value = invocation->context->x[location.register_index];
        return true;
    case dartplant::abi::DartAbiLocationKind::kFpuRegister:
        if (location.register_index >= 32) return false;
        std::memcpy(out_value, invocation->context->v[location.register_index], sizeof(*out_value));
        return true;
    case dartplant::abi::DartAbiLocationKind::kEntryStack: {
        if (invocation->call_layout == nullptr || invocation->call_layout->dart_sp_register >= 31 ||
            location.stack_offset < 0) {
            return false;
        }
        // Dart AOT stack parameter locations are relative to the VM's Dart
        // stack register (SPREG=x15 on ARM64), not the architectural SP used
        // by the native bridge's own save frame. PRODUCT code confirms this
        // directly with x15-relative loads and with framed callees that set
        // FPREG from SPREG before reading fp-relative parameters.
        const uintptr_t base = invocation->context->x[invocation->call_layout->dart_sp_register];
        if (base == 0) return false;
        std::memcpy(out_value, reinterpret_cast<const void*>(base + location.stack_offset),
                    sizeof(*out_value));
        return true;
    }
    case dartplant::abi::DartAbiLocationKind::kUnknown:
        return false;
    }
    return false;
}

bool WriteVerifiedLocation(DartPlantInvocation* invocation,
                           const dartplant::abi::DartAbiLocation& location, uint64_t value) {
    if (invocation == nullptr || invocation->context == nullptr) return false;
    switch (location.kind) {
    case dartplant::abi::DartAbiLocationKind::kGpRegister:
        if (!ValidRegister(location.register_index)) return false;
        invocation->context->x[location.register_index] = value;
        return true;
    case dartplant::abi::DartAbiLocationKind::kFpuRegister:
        if (location.register_index >= 32) return false;
        std::memcpy(invocation->context->v[location.register_index], &value, sizeof(value));
        return true;
    case dartplant::abi::DartAbiLocationKind::kEntryStack: {
        if (invocation->call_layout == nullptr || invocation->call_layout->dart_sp_register >= 31 ||
            location.stack_offset < 0) {
            return false;
        }
        const uintptr_t base = invocation->context->x[invocation->call_layout->dart_sp_register];
        if (base == 0) return false;
        std::memcpy(reinterpret_cast<void*>(base + location.stack_offset), &value, sizeof(value));
        return true;
    }
    case dartplant::abi::DartAbiLocationKind::kUnknown:
        return false;
    }
    return false;
}

DartPlantStatus DecodeVerifiedValue(const DartPlantInvocation* invocation,
                                    const dartplant::abi::DartParameterLayout& layout,
                                    DartPlantValue* out_value) {
    if (layout.representation == dartplant::abi::DartAbiRepresentation::kPairOfTagged) {
        dartplant::SetLastError("pair-of-tagged result requires the pair result API");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (out_value == nullptr || layout.location.count != 1 ||
        layout.location.locations[0].kind == dartplant::abi::DartAbiLocationKind::kUnknown) {
        dartplant::SetLastError("verified DartCallLayout slot is invalid");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    uint64_t raw = 0;
    if (!ReadVerifiedLocation(invocation, layout.location.locations[0], &raw)) {
        dartplant::SetLastError("verified DartCallLayout location is unreadable");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    switch (layout.representation) {
    case dartplant::abi::DartAbiRepresentation::kTagged:
        *out_value = RefineTaggedSemanticValue(
            invocation, dartplant::dartplant_vm_abi_decode_gp_word(
                            raw, true, ActiveValidatedNullValue(invocation)));
        return DARTPLANT_OK;
    case dartplant::abi::DartAbiRepresentation::kUnboxedInt64:
        *out_value = {DARTPLANT_VALUE_INT64, 0, raw};
        return DARTPLANT_OK;
    case dartplant::abi::DartAbiRepresentation::kUnboxedDouble:
        *out_value = dartplant::dartplant_vm_abi_decode_fp_word(raw);
        return DARTPLANT_OK;
    case dartplant::abi::DartAbiRepresentation::kPairOfTagged:
        return DARTPLANT_UNSUPPORTED_ABI;
    case dartplant::abi::DartAbiRepresentation::kUnknown:
        dartplant::SetLastError("verified DartCallLayout representation is unknown");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    return DARTPLANT_UNSUPPORTED_ABI;
}

DartPlantStatus EncodeVerifiedValue(DartPlantInvocation* invocation,
                                    const dartplant::abi::DartParameterLayout& layout,
                                    const DartPlantValue* value) {
    if (layout.representation == dartplant::abi::DartAbiRepresentation::kPairOfTagged) {
        dartplant::SetLastError("pair-of-tagged result requires the pair result API");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (value == nullptr || layout.location.count != 1 ||
        layout.location.locations[0].kind == dartplant::abi::DartAbiLocationKind::kUnknown) {
        dartplant::SetLastError("verified DartCallLayout slot is invalid");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    uint64_t raw = 0;
    DartPlantStatus status = DARTPLANT_OK;
    switch (layout.representation) {
    case dartplant::abi::DartAbiRepresentation::kTagged:
        status = EncodeGpSemanticValue(invocation, value, true, &raw);
        break;
    case dartplant::abi::DartAbiRepresentation::kUnboxedInt64:
        if (value->kind != DARTPLANT_VALUE_INT64) {
            dartplant::SetLastError("unboxed int64 locations require DARTPLANT_VALUE_INT64");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        raw = value->raw;
        break;
    case dartplant::abi::DartAbiRepresentation::kUnboxedDouble:
        if (value->kind != DARTPLANT_VALUE_DOUBLE) {
            dartplant::SetLastError("unboxed double locations require DARTPLANT_VALUE_DOUBLE");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        raw = value->raw;
        break;
    case dartplant::abi::DartAbiRepresentation::kPairOfTagged:
        return DARTPLANT_UNSUPPORTED_ABI;
    case dartplant::abi::DartAbiRepresentation::kUnknown:
        dartplant::SetLastError("verified DartCallLayout representation is unknown");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (status != DARTPLANT_OK) return status;
    if (!WriteVerifiedLocation(invocation, layout.location.locations[0], raw)) {
        dartplant::SetLastError("verified DartCallLayout location is unwritable");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    return DARTPLANT_OK;
}

DartPlantStatus DecodeVerifiedTaggedPair(const DartPlantInvocation* invocation,
                                         const dartplant::abi::DartParameterLayout& layout,
                                         DartPlantValuePair* out_value) {
    if (invocation == nullptr || out_value == nullptr ||
        layout.representation != dartplant::abi::DartAbiRepresentation::kPairOfTagged ||
        layout.location.count != 2) {
        dartplant::SetLastError("verified pair result layout is invalid");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    uint64_t first_raw = 0;
    uint64_t second_raw = 0;
    if (!ReadVerifiedLocation(invocation, layout.location.locations[0], &first_raw) ||
        !ReadVerifiedLocation(invocation, layout.location.locations[1], &second_raw)) {
        dartplant::SetLastError("verified pair result location is unreadable");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    out_value->first = RefineTaggedSemanticValue(
        invocation, dartplant::dartplant_vm_abi_decode_gp_word(
                        first_raw, true, ActiveValidatedNullValue(invocation)));
    out_value->second = RefineTaggedSemanticValue(
        invocation, dartplant::dartplant_vm_abi_decode_gp_word(
                        second_raw, true, ActiveValidatedNullValue(invocation)));
    return DARTPLANT_OK;
}

DartPlantStatus EncodeVerifiedTaggedPair(DartPlantInvocation* invocation,
                                         const dartplant::abi::DartParameterLayout& layout,
                                         const DartPlantValuePair* value) {
    if (invocation == nullptr || value == nullptr ||
        layout.representation != dartplant::abi::DartAbiRepresentation::kPairOfTagged ||
        layout.location.count != 2) {
        dartplant::SetLastError("verified pair result layout is invalid");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    uint64_t first_raw = 0;
    uint64_t second_raw = 0;
    DartPlantStatus status = EncodeGpSemanticValue(invocation, &value->first, true, &first_raw);
    if (status != DARTPLANT_OK) return status;
    status = EncodeGpSemanticValue(invocation, &value->second, true, &second_raw);
    if (status != DARTPLANT_OK) return status;
    if (!WriteVerifiedLocation(invocation, layout.location.locations[0], first_raw) ||
        !WriteVerifiedLocation(invocation, layout.location.locations[1], second_raw)) {
        dartplant::SetLastError("verified pair result location is unwritable");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    return DARTPLANT_OK;
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

uint8_t dartplant_invocation_has_verified_abi(const DartPlantInvocation* invocation) {
    return HasVerifiedCallLayout(invocation) ? 1 : 0;
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
    if (!ArgumentAccessEnabled(invocation)) return 0;
    return HasVerifiedCallLayout(invocation)
               ? static_cast<uint32_t>(invocation->call_layout->parameters.size())
               : invocation->profile->argument_count;
}

uint8_t dartplant_invocation_has_closure_receiver(const DartPlantInvocation* invocation) {
    return ClosureReceiverAccessEnabled(invocation) ? 1 : 0;
}

DartPlantStatus dartplant_invocation_get_closure_receiver(const DartPlantInvocation* invocation,
                                                          DartPlantValue* out_value) {
    if (!ClosureReceiverAccessEnabled(invocation) || invocation->context == nullptr ||
        out_value == nullptr) {
        dartplant::SetLastError("verified closure receiver is unavailable");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_ENTER) {
        dartplant::SetLastError("closure receiver is only available during enter");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    uint64_t raw = 0;
    if (!ReadClosureReceiverRaw(invocation, &raw)) {
        dartplant::SetLastError("verified closure receiver location is unreadable");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    *out_value =
        RefineTaggedSemanticValue(invocation, dartplant::dartplant_vm_abi_decode_gp_word(
                                                  raw, true, ActiveValidatedNullValue(invocation)));
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_get_arguments_descriptor(
    const DartPlantInvocation* invocation, DartPlantArgumentsDescriptorInfo* out_info) {
    if (invocation == nullptr || invocation->context == nullptr || out_info == nullptr ||
        out_info->struct_size < sizeof(DartPlantArgumentsDescriptorInfo) ||
        !HasVerifiedCallLayout(invocation) || !invocation->call_layout->has_arguments_descriptor ||
        invocation->requested_method == nullptr ||
        invocation->requested_method->function == nullptr) {
        dartplant::SetLastError("verified closure ArgumentsDescriptor is unavailable");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_ENTER) {
        dartplant::SetLastError("closure ArgumentsDescriptor is only available during enter");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    const auto* profile = dartplant::FindRuntimeProfileByVersion(
        invocation->requested_method->function->runtime_profile_version);
    if (profile == nullptr) {
        dartplant::SetLastError("closure ArgumentsDescriptor has no exact runtime profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    uint64_t raw = 0;
    const auto& raw_layout = profile->raw_object;
    if (!ReadVerifiedLocation(invocation, invocation->call_layout->arguments_descriptor_location,
                              &raw) ||
        raw == 0 || (raw & raw_layout.smi_tag_mask) != raw_layout.heap_object_tag ||
        raw < raw_layout.heap_object_tag) {
        dartplant::SetLastError("closure ArgumentsDescriptor register is not a tagged object");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (raw_layout.class_id_tag_bits == 0 || raw_layout.class_id_tag_bits >= 64 ||
        raw_layout.class_id_tag_shift >= 64 - raw_layout.class_id_tag_bits) {
        dartplant::SetLastError("closure ArgumentsDescriptor raw-object profile is invalid");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const uint64_t class_id_mask = (uint64_t{1} << raw_layout.class_id_tag_bits) - 1;
    const uintptr_t object = static_cast<uintptr_t>(raw - raw_layout.heap_object_tag);
    uint64_t tags = 0;
    if (!ReadSelfValue(object, &tags)) {
        dartplant::SetLastError("closure ArgumentsDescriptor object is unreadable");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const uint32_t cid =
        static_cast<uint32_t>((tags >> raw_layout.class_id_tag_shift) & class_id_mask);
    if (cid != profile->live_vm.cid_array && cid != profile->live_vm.cid_immutable_array) {
        dartplant::SetLastError("closure ArgumentsDescriptor is not a Dart Array");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    DartPlantArgumentsDescriptorInfo info{};
    info.struct_size = sizeof(info);
    info.raw_descriptor = raw;
    const auto& layout = profile->arguments_descriptor;
    if (!ReadPositiveCompressedSmi(object + layout.type_args_len_offset, raw_layout,
                                   &info.type_args_len) ||
        !ReadPositiveCompressedSmi(object + layout.count_offset, raw_layout, &info.count) ||
        !ReadPositiveCompressedSmi(object + layout.size_offset, raw_layout, &info.size) ||
        !ReadPositiveCompressedSmi(object + layout.positional_count_offset, raw_layout,
                                   &info.positional_count) ||
        info.positional_count > info.count || info.count > info.size) {
        dartplant::SetLastError("closure ArgumentsDescriptor counters are inconsistent");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (invocation->call_layout->has_closure_receiver) {
        // Kernel flowgraph construction includes the Closure receiver in the
        // call arguments, then adds a second explicit Closure input used only
        // to load the target entry. TemplateDartCall<1> excludes that target
        // input, not the receiver argument, from ArgumentsDescriptor. Typed
        // Function formals deliberately expose neither hidden value.
        const uint64_t formal_count = invocation->call_layout->parameters.size();
        if (formal_count >= UINT32_MAX) {
            dartplant::SetLastError("verified closure formal count is invalid");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        const uint64_t expected_argument_count = formal_count + 1;
        if (info.count != expected_argument_count || info.size != expected_argument_count ||
            info.positional_count != expected_argument_count) {
            dartplant::SetLastError(
                "closure ArgumentsDescriptor does not match the verified fixed-arity call layout");
            return DARTPLANT_PROFILE_MISMATCH;
        }
    }
    info.named_count = info.count - info.positional_count;
    *out_info = info;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_get_argument(const DartPlantInvocation* invocation,
                                                  uint32_t index, DartPlantValue* out_value) {
    const uint32_t argument_count = dartplant_invocation_argument_count(invocation);
    if (!ArgumentAccessEnabled(invocation) || invocation->context == nullptr ||
        out_value == nullptr || index >= argument_count) {
        dartplant::SetLastError("argument decoding is not enabled by the profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (HasVerifiedCallLayout(invocation)) {
        return DecodeVerifiedValue(invocation, invocation->call_layout->parameters[index],
                                   out_value);
    }
    const auto& location = invocation->profile->argument_locations[index];
    uint64_t raw = 0;
    if (!ReadLocation(invocation, location, &raw)) {
        dartplant::SetLastError("profile argument register is invalid");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    *out_value = location.kind == DARTPLANT_ABI_FP_REGISTER
                     ? dartplant::dartplant_vm_abi_decode_fp_word(raw)
                     : RefineTaggedSemanticValue(
                           invocation, dartplant::dartplant_vm_abi_decode_gp_word(
                                           raw, LegacyTaggedArgumentAccessEnabled(invocation),
                                           ActiveValidatedNullValue(invocation)));
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_set_argument(DartPlantInvocation* invocation, uint32_t index,
                                                  const DartPlantValue* value) {
    const uint32_t argument_count = dartplant_invocation_argument_count(invocation);
    if (!ArgumentAccessEnabled(invocation) || invocation->context == nullptr || value == nullptr ||
        index >= argument_count) {
        dartplant::SetLastError("argument encoding is not enabled by the profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_ENTER) {
        dartplant::SetLastError("arguments can only be changed during enter");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    if (HasVerifiedCallLayout(invocation)) {
        return EncodeVerifiedValue(invocation, invocation->call_layout->parameters[index], value);
    }
    const auto& location = invocation->profile->argument_locations[index];
    uint64_t raw = 0;
    const DartPlantStatus status =
        location.kind == DARTPLANT_ABI_FP_REGISTER
            ? dartplant::dartplant_vm_abi_encode_fp_word(value, &raw)
            : EncodeGpSemanticValue(invocation, value,
                                    LegacyTaggedArgumentAccessEnabled(invocation), &raw);
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
    const uint32_t argument_count = dartplant_invocation_argument_count(invocation);
    if (!ArgumentIsTagged(invocation, index) || invocation->context == nullptr ||
        index >= argument_count || out_handle == nullptr) {
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
    uint64_t raw = 0;
    if (HasVerifiedCallLayout(invocation)) {
        const auto& layout = invocation->call_layout->parameters[index];
        if (layout.location.count != 1 ||
            !ReadVerifiedLocation(invocation, layout.location.locations[0], &raw)) {
            dartplant::SetLastError("verified tagged argument location is invalid");
            return DARTPLANT_PROFILE_MISMATCH;
        }
    } else {
        const auto& location = invocation->profile->argument_locations[index];
        if (location.kind == DARTPLANT_ABI_FP_REGISTER) {
            dartplant::SetLastError("FP arguments cannot be retained as VM objects");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        if (!ReadLocation(invocation, location, &raw)) {
            dartplant::SetLastError("profile argument register is invalid");
            return DARTPLANT_PROFILE_MISMATCH;
        }
    }
    return dartplant::VmAdapterRetainObject(invocation->vm_adapter, raw, strength, out_handle);
}

DartPlantStatus dartplant_invocation_set_argument_object(DartPlantInvocation* invocation,
                                                         uint32_t index,
                                                         const DartPlantObjectHandle* handle) {
    const uint32_t argument_count = dartplant_invocation_argument_count(invocation);
    if (!ArgumentIsTagged(invocation, index) || invocation->context == nullptr ||
        handle == nullptr || index >= argument_count) {
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
    const DartPlantStatus valid = dartplant::VmAdapterCheckHandle(invocation->vm_adapter, handle);
    if (valid != DARTPLANT_OK) return valid;
    uint64_t raw = 0;
    const DartPlantStatus status = dartplant::VmAdapterSetRaw(handle, &raw);
    if (status != DARTPLANT_OK) return status;
    if (HasVerifiedCallLayout(invocation)) {
        const auto& layout = invocation->call_layout->parameters[index];
        if (layout.location.count != 1 ||
            !WriteVerifiedLocation(invocation, layout.location.locations[0], raw)) {
            return DARTPLANT_PROFILE_MISMATCH;
        }
    } else {
        const auto& location = invocation->profile->argument_locations[index];
        if (location.kind == DARTPLANT_ABI_FP_REGISTER) {
            dartplant::SetLastError("FP arguments cannot be set from VM objects");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        if (!WriteLocation(invocation, location, raw)) return DARTPLANT_PROFILE_MISMATCH;
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
    if (HasVerifiedCallLayout(invocation)) {
        return DecodeVerifiedValue(invocation, invocation->call_layout->result, out_value);
    }
    uint64_t raw = 0;
    if (!ReadLocation(invocation, invocation->profile->result_location, &raw)) {
        return DARTPLANT_PROFILE_MISMATCH;
    }
    *out_value = invocation->profile->result_location.kind == DARTPLANT_ABI_FP_REGISTER
                     ? dartplant::dartplant_vm_abi_decode_fp_word(raw)
                     : RefineTaggedSemanticValue(
                           invocation, dartplant::dartplant_vm_abi_decode_gp_word(
                                           raw, LegacyTaggedResultAccessEnabled(invocation),
                                           ActiveValidatedNullValue(invocation)));
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_set_result(DartPlantInvocation* invocation,
                                                const DartPlantValue* value) {
    if (!ResultAccessEnabled(invocation) || invocation->context == nullptr || value == nullptr) {
        dartplant::SetLastError("result encoding is not enabled by the profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (HasVerifiedCallLayout(invocation)) {
        const DartPlantStatus status =
            EncodeVerifiedValue(invocation, invocation->call_layout->result, value);
        if (status != DARTPLANT_OK) return status;
    } else {
        uint64_t raw = 0;
        const DartPlantStatus status =
            invocation->profile->result_location.kind == DARTPLANT_ABI_FP_REGISTER
                ? dartplant::dartplant_vm_abi_encode_fp_word(value, &raw)
                : EncodeGpSemanticValue(invocation, value,
                                        LegacyTaggedResultAccessEnabled(invocation), &raw);
        if (status != DARTPLANT_OK) return status;
        if (!WriteLocation(invocation, invocation->profile->result_location, raw)) {
            return DARTPLANT_PROFILE_MISMATCH;
        }
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

DartPlantStatus dartplant_invocation_get_result_pair(const DartPlantInvocation* invocation,
                                                     DartPlantValuePair* out_value) {
    if (!HasVerifiedCallLayout(invocation) || invocation->context == nullptr ||
        out_value == nullptr) {
        dartplant::SetLastError("pair result decoding requires a verified DartCallLayout");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_LEAVE && !invocation->skip_original) {
        dartplant::SetLastError(
            "pair result is only available during leave or after skipping original");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    return DecodeVerifiedTaggedPair(invocation, invocation->call_layout->result, out_value);
}

DartPlantStatus dartplant_invocation_set_result_pair(DartPlantInvocation* invocation,
                                                     const DartPlantValuePair* value) {
    if (!HasVerifiedCallLayout(invocation) || invocation->context == nullptr || value == nullptr) {
        dartplant::SetLastError("pair result encoding requires a verified DartCallLayout");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (invocation->phase != DARTPLANT_INVOCATION_ENTER &&
        invocation->phase != DARTPLANT_INVOCATION_LEAVE) {
        dartplant::SetLastError("pair result cannot be changed in this phase");
        return DARTPLANT_INVALID_INVOCATION_PHASE;
    }
    const DartPlantStatus status =
        EncodeVerifiedTaggedPair(invocation, invocation->call_layout->result, value);
    if (status != DARTPLANT_OK) return status;
    if (invocation->phase == DARTPLANT_INVOCATION_ENTER) {
        invocation->skip_original = true;
        invocation->call_original = false;
    }
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_invocation_retain_result_object(DartPlantInvocation* invocation,
                                                          DartPlantObjectStrength strength,
                                                          DartPlantObjectHandle** out_handle) {
    if (!ResultIsTagged(invocation) || invocation->context == nullptr || out_handle == nullptr) {
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
    uint64_t raw = 0;
    if (HasVerifiedCallLayout(invocation)) {
        const auto& layout = invocation->call_layout->result;
        if (layout.location.count != 1 ||
            !ReadVerifiedLocation(invocation, layout.location.locations[0], &raw)) {
            return DARTPLANT_PROFILE_MISMATCH;
        }
    } else {
        const auto& location = invocation->profile->result_location;
        if (location.kind == DARTPLANT_ABI_FP_REGISTER) {
            dartplant::SetLastError("FP results cannot be retained as VM objects");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        if (!ReadLocation(invocation, location, &raw)) return DARTPLANT_PROFILE_MISMATCH;
    }
    return dartplant::VmAdapterRetainObject(invocation->vm_adapter, raw, strength, out_handle);
}

DartPlantStatus dartplant_invocation_set_result_object(DartPlantInvocation* invocation,
                                                       const DartPlantObjectHandle* handle) {
    if (!ResultIsTagged(invocation) || invocation->context == nullptr || handle == nullptr) {
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
    const DartPlantStatus valid = dartplant::VmAdapterCheckHandle(invocation->vm_adapter, handle);
    if (valid != DARTPLANT_OK) return valid;
    uint64_t raw = 0;
    const DartPlantStatus status = dartplant::VmAdapterSetRaw(handle, &raw);
    if (status != DARTPLANT_OK) return status;
    if (HasVerifiedCallLayout(invocation)) {
        const auto& layout = invocation->call_layout->result;
        if (layout.location.count != 1 ||
            !WriteVerifiedLocation(invocation, layout.location.locations[0], raw)) {
            return DARTPLANT_PROFILE_MISMATCH;
        }
    } else {
        const auto& location = invocation->profile->result_location;
        if (location.kind == DARTPLANT_ABI_FP_REGISTER) {
            dartplant::SetLastError("FP results cannot be set from VM objects");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        if (!WriteLocation(invocation, location, raw)) return DARTPLANT_PROFILE_MISMATCH;
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
    if (invocation->hook == nullptr ||
        invocation->hook->backup.load(std::memory_order_acquire) == nullptr) {
        dartplant::SetLastError("original entry is unavailable");
        return DARTPLANT_HOOK_FAILED;
    }
    if (invocation->generated_vm_bridge_active) {
        // The callback currently owns a VM-visible Generated->Native
        // transition. Re-entering Dart synchronously would require leaving the
        // API scope, returning the Thread to generated state, refreshing every
        // rooted tagged location, then rebuilding the transition when the
        // original returns. Until that nested protocol is implemented, keep
        // automatic original execution safe and make explicit call_original()
        // fail closed.
        dartplant::SetLastError(
            "synchronous original invocation is unavailable inside a generated/native VM bridge");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    if (dartplant_arm64_invoke_original(
            invocation->context, invocation->hook->backup.load(std::memory_order_acquire)) == 0) {
        if (dartplant_last_error()[0] == '\0') {
            dartplant::SetLastError("synchronous original invocation bridge failed");
        }
        return DARTPLANT_HOOK_FAILED;
    }
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
