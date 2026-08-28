// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ABI_VALUE_CODEC_H_
#define DARTPLANT_ABI_VALUE_CODEC_H_

#include <stdint.h>

#include "dartplant/invocation.h"

namespace dartplant {

enum : uint64_t {
    DARTPLANT_VM_TAG_MASK = 1u,
    DARTPLANT_VM_HEAP_OBJECT_TAG = 1u,
};

bool dartplant_vm_abi_is_tagged_smi(uint64_t raw);
bool dartplant_vm_abi_is_tagged_heap_object(uint64_t raw);

DartPlantValue dartplant_vm_abi_decode_gp_word(uint64_t raw, bool is_tagged,
                                               uint64_t validated_null_value);
DartPlantValue dartplant_vm_abi_decode_fp_word(uint64_t raw);

DartPlantStatus dartplant_vm_abi_encode_gp_word(const DartPlantValue* value, bool is_tagged,
                                                uint64_t validated_null_value, uint64_t* out_raw);
DartPlantStatus dartplant_vm_abi_encode_fp_word(const DartPlantValue* value, uint64_t* out_raw);

}  // namespace dartplant

#endif  // DARTPLANT_ABI_VALUE_CODEC_H_
