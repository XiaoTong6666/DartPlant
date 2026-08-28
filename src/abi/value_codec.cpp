#include "abi/value_codec.h"

#include "core/internal.h"

namespace dartplant {

bool dartplant_vm_abi_is_tagged_smi(uint64_t raw) { return (raw & DARTPLANT_VM_TAG_MASK) == 0; }

bool dartplant_vm_abi_is_tagged_heap_object(uint64_t raw) {
    return (raw & DARTPLANT_VM_TAG_MASK) == DARTPLANT_VM_HEAP_OBJECT_TAG;
}

DartPlantValue dartplant_vm_abi_decode_gp_word(uint64_t raw, bool is_tagged,
                                               uint64_t validated_null_value) {
    if (!is_tagged) return {DARTPLANT_VALUE_RAW_WORD, 0, raw};
    if (validated_null_value != 0 && raw == validated_null_value) {
        return {DARTPLANT_VALUE_NULL, 0, raw};
    }
    return {dartplant_vm_abi_is_tagged_heap_object(raw) ? DARTPLANT_VALUE_HEAP_OBJECT
                                                        : DARTPLANT_VALUE_SMI,
            0, raw};
}

DartPlantValue dartplant_vm_abi_decode_fp_word(uint64_t raw) {
    return {DARTPLANT_VALUE_DOUBLE, 0, raw};
}

DartPlantStatus dartplant_vm_abi_encode_gp_word(const DartPlantValue* value, bool is_tagged,
                                                uint64_t validated_null_value, uint64_t* out_raw) {
    if (value == nullptr || out_raw == nullptr) {
        SetLastError("GP value encoding arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (!is_tagged) {
        if (value->kind != DARTPLANT_VALUE_RAW_WORD) {
            SetLastError("raw GP locations only accept DARTPLANT_VALUE_RAW_WORD");
            return DARTPLANT_UNSUPPORTED_ABI;
        }
        *out_raw = value->raw;
        return DARTPLANT_OK;
    }
    if (value->kind == DARTPLANT_VALUE_NULL) {
        if (validated_null_value == 0) {
            SetLastError("null value encoding requires a validated Dart NULL_REG value");
            return DARTPLANT_UNSUPPORTED_ABI;
        }
        *out_raw = validated_null_value;
        return DARTPLANT_OK;
    }
    if (value->kind == DARTPLANT_VALUE_SMI && !dartplant_vm_abi_is_tagged_smi(value->raw)) {
        SetLastError("SMI values must use an untagged raw word");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (value->kind == DARTPLANT_VALUE_HEAP_OBJECT &&
        !dartplant_vm_abi_is_tagged_heap_object(value->raw)) {
        SetLastError("heap object values must use a tagged raw word");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (value->kind == DARTPLANT_VALUE_BOOL || value->kind == DARTPLANT_VALUE_DOUBLE ||
        value->kind == DARTPLANT_VALUE_INT64) {
        SetLastError("GP value kind is not supported by the validated Dart ABI profile");
        return DARTPLANT_UNSUPPORTED_ABI;
    }
    *out_raw = value->raw;
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_vm_abi_encode_fp_word(const DartPlantValue* value, uint64_t* out_raw) {
    if (value == nullptr || out_raw == nullptr) {
        SetLastError("FP value encoding arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (value->kind != DARTPLANT_VALUE_DOUBLE && value->kind != DARTPLANT_VALUE_RAW_WORD &&
        value->kind != DARTPLANT_VALUE_UNKNOWN) {
        SetLastError("FP values must be encoded as a raw double word");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    *out_raw = value->raw;
    return DARTPLANT_OK;
}

}  // namespace dartplant
