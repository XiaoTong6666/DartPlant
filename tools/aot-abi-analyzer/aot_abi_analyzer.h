// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_TOOLS_AOT_ABI_ANALYZER_AOT_ABI_ANALYZER_H_
#define DARTPLANT_TOOLS_AOT_ABI_ANALYZER_AOT_ABI_ANALYZER_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "abi/evidence.h"

namespace dartplant::aot {

using ParameterEvidence = abi::DartAbiRepresentation;
using LocationKind = abi::DartAbiLocationKind;
using AbiLocation = abi::DartAbiLocation;
using AbiObservation = abi::DartAbiObservation;

constexpr size_t kMaxAbiObservations = 32;

struct AnalysisResult {
    // Kept as a compact compatibility view for the original analyzer tests.
    std::array<ParameterEvidence, 6> fpu_arguments{};
    std::array<AbiObservation, kMaxAbiObservations> observations{};
    size_t observation_count = 0;
    ParameterEvidence return_evidence = ParameterEvidence::kUnknown;
    size_t decoded_instructions = 0;
    size_t basic_block_count = 0;
    bool stopped_at_control_flow = false;
    bool has_unknown_control_flow = false;
    bool uses_arguments_descriptor = false;
    bool reached_return = false;
};

// Analyze an ARM64 entry without relying on semantic Dart types. The analyzer
// proves only physical value flow from incoming GP/FPU/entry-stack locations;
// unresolved aliases, CFG merges and indirect control flow remain unknown.
AnalysisResult AnalyzeArm64Entry(const uint8_t* code, size_t size, uint64_t address = 0);

const AbiObservation* FindObservation(const AnalysisResult& result, const AbiLocation& location);

}  // namespace dartplant::aot

#endif  // DARTPLANT_TOOLS_AOT_ABI_ANALYZER_AOT_ABI_ANALYZER_H_
