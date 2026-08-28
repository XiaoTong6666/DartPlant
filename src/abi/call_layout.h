// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ABI_CALL_LAYOUT_H_
#define DARTPLANT_ABI_CALL_LAYOUT_H_

#include <stdint.h>

#include <array>
#include <vector>

#include "abi/representation.h"

namespace dartplant::abi {

enum class DartCallLayoutStatus : uint8_t {
    kOk = 0,
    kIncompleteEvidence,
    kConflictingEvidence,
    kUnsupportedRepresentation,
    kOptionalArgumentsUnsupported,
    kInvalidCallingConvention,
};

enum class DartAbiLocationKind : uint8_t {
    kUnknown = 0,
    kGpRegister,
    kFpuRegister,
    kEntryStack,
};

struct DartAbiLocation {
    DartAbiLocationKind kind = DartAbiLocationKind::kUnknown;
    uint8_t register_index = 0;
    int32_t stack_offset = 0;

    friend bool operator==(const DartAbiLocation &, const DartAbiLocation &) = default;
};

struct DartAbiValueLocation {
    std::array<DartAbiLocation, 2> locations{};
    uint8_t count = 0;
};

struct DartParameterLayout {
    DartAbiRepresentation representation = DartAbiRepresentation::kUnknown;
    DartAbiValueLocation location{};
};

struct DartCallLayout {
    std::vector<DartParameterLayout> parameters;
    DartParameterLayout result{};
    uint32_t stack_words = 0;
    uint8_t dart_sp_register = 0;
};

}  // namespace dartplant::abi

#endif  // DARTPLANT_ABI_CALL_LAYOUT_H_
