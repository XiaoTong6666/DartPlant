// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_VM_ABI_PROBE_H_
#define DARTPLANT_VM_ABI_PROBE_H_

#include <cstdint>
#include <vector>

#include "core/internal.h"
#include "vm/abi/proof.h"

namespace dartplant::vm_abi {

enum class CandidateProbeStage : uint8_t {
    kNotStarted = 0,
    kRuntimeRoots,
    kEnterSafepoint,
    kExitSafepoint,
    kSafepointModuleRelation,
    kComplete,
};

const char* CandidateProbeStageName(const CandidateProbeStage& stage, const RootProof& roots);

struct CandidateProbeInput {
    RootProofInput roots{};
    const std::vector<ModuleImage>* modules = nullptr;
};

struct CandidateProbe {
    CandidateProbeStage stage = CandidateProbeStage::kNotStarted;
    RootProof roots{};
    uint64_t enter_code = 0;
    uint64_t exit_code = 0;
    uint64_t enter_entry = 0;
    uint64_t exit_entry = 0;
    const ModuleImage* code_module = nullptr;
    bool passed = false;
};

CandidateProbe ProbeCandidate(const CandidateProbeInput& input);

}  // namespace dartplant::vm_abi

#endif  // DARTPLANT_VM_ABI_PROBE_H_
