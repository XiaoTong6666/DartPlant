// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ABI_REPRESENTATION_H_
#define DARTPLANT_ABI_REPRESENTATION_H_

#include <stdint.h>

namespace dartplant::abi {

// Dart source-language types and machine representations are deliberately
// separate concepts. This enum mirrors only the representations that are
// currently modeled at a Dart AOT function boundary.
enum class DartAbiRepresentation : uint8_t {
    kUnknown = 0,
    kTagged,
    kUnboxedInt64,
    kUnboxedDouble,
    kPairOfTagged,
};

const char* AbiRepresentationName(DartAbiRepresentation representation);

}  // namespace dartplant::abi

#endif  // DARTPLANT_ABI_REPRESENTATION_H_
