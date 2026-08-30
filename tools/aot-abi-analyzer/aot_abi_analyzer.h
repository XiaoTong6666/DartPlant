// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_TOOLS_AOT_ABI_ANALYZER_AOT_ABI_ANALYZER_H_
#define DARTPLANT_TOOLS_AOT_ABI_ANALYZER_AOT_ABI_ANALYZER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "abi/evidence.h"

namespace dartplant::aot {

using ParameterEvidence = abi::DartAbiRepresentation;
using LocationKind = abi::DartAbiLocationKind;
using AbiLocation = abi::DartAbiLocation;
using AbiObservation = abi::DartAbiObservation;

constexpr size_t kMaxAbiObservations = 32;
constexpr size_t kMaxStructuralSites = 64;

struct DirectControlFlowEdge {
    uint64_t site = 0;
    uint64_t target = 0;
};

enum class AddressMaterializationKind : uint8_t {
    kAdr = 0,
    kAdrpAdd,
};

struct AddressMaterialization {
    AddressMaterializationKind kind = AddressMaterializationKind::kAdr;
    uint8_t destination_register = 0;
    uint16_t reserved = 0;
    uint64_t site = 0;
    uint64_t completion_site = 0;
    uint64_t target = 0;
};

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
    bool structural_evidence_truncated = false;
    std::array<DirectControlFlowEdge, kMaxStructuralSites> direct_calls{};
    size_t direct_call_count = 0;
    std::array<uint64_t, kMaxStructuralSites> return_sites{};
    size_t return_site_count = 0;
    std::array<DirectControlFlowEdge, kMaxStructuralSites> external_branches{};
    size_t external_branch_count = 0;
    std::array<uint64_t, kMaxStructuralSites> indirect_call_sites{};
    size_t indirect_call_count = 0;
    std::array<uint64_t, kMaxStructuralSites> indirect_branch_sites{};
    size_t indirect_branch_count = 0;
    std::array<AddressMaterialization, kMaxStructuralSites> address_materializations{};
    size_t address_materialization_count = 0;
};

// Generic relationship proof for already identity-bound Dart entry targets.
// Candidate discovery and relationship proof are deliberately separate: scan
// order is never used to pick a winner, and zero or multiple matches fail
// closed. This is the reusable form of the relationship/uniqueness technique
// used by MiuiBackGestureHook's paired Dart callback resolver.
enum class StructuralRelationKind : uint8_t {
    kDirectCallTarget = 0,
    kAddressTarget,
    kExternalBranchTarget,
    kReturnSite,
    kUsesArgumentsDescriptor,
    kNoUnknownControlFlow,
};

struct StructuralRelationConstraint {
    StructuralRelationKind kind = StructuralRelationKind::kDirectCallTarget;
    uint64_t value = 0;
    uint32_t min_count = 1;
    uint32_t max_count = UINT32_MAX;
};

struct StructuralCandidate {
    uint64_t id = 0;
    const AnalysisResult* analysis = nullptr;
};

enum class StructuralSelectionStatus : uint8_t {
    kNoMatch = 0,
    kUnique,
    kAmbiguous,
};

struct StructuralSelection {
    StructuralSelectionStatus status = StructuralSelectionStatus::kNoMatch;
    size_t candidate_count = 0;
    size_t matching_count = 0;
    uint64_t selected_id = 0;
};

// Analyze an ARM64 entry without relying on semantic Dart types. The analyzer
// proves only physical value flow from incoming GP/FPU/entry-stack locations;
// unresolved aliases, CFG merges and indirect control flow remain unknown.
AnalysisResult AnalyzeArm64Entry(const uint8_t* code, size_t size, uint64_t address = 0);

const AbiObservation* FindObservation(const AnalysisResult& result, const AbiLocation& location);

StructuralSelection SelectUniqueStructuralCandidate(
    std::span<const StructuralCandidate> candidates,
    std::span<const StructuralRelationConstraint> constraints);

}  // namespace dartplant::aot

#endif  // DARTPLANT_TOOLS_AOT_ABI_ANALYZER_AOT_ABI_ANALYZER_H_
