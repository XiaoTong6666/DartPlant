// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "abi/representation.h"

namespace dartplant::abi {

const char* AbiRepresentationName(DartAbiRepresentation representation) {
    switch (representation) {
    case DartAbiRepresentation::kUnknown:
        return "unknown";
    case DartAbiRepresentation::kTagged:
        return "tagged";
    case DartAbiRepresentation::kUnboxedInt64:
        return "unboxed-int64";
    case DartAbiRepresentation::kUnboxedDouble:
        return "unboxed-double";
    case DartAbiRepresentation::kPairOfTagged:
        return "pair-of-tagged";
    }
    return "unknown";
}

}  // namespace dartplant::abi
