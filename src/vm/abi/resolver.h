// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_VM_ABI_RESOLVER_H_
#define DARTPLANT_VM_ABI_RESOLVER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "vm/abi/probe.h"

namespace dartplant::vm_abi {

enum VmCapability : uint64_t {
    kCapabilityNone = 0,
    kCapabilityRuntimeRoots = 1ULL << 0,
    kCapabilityOwnerIdentity = 1ULL << 1,
    kCapabilityCanonicalNull = 1ULL << 2,
    kCapabilityRegisterSemantics = 1ULL << 3,
    kCapabilityDartCore = 1ULL << 4,
    kCapabilitySafepointStubs = 1ULL << 5,
    kCapabilityGeneratedTransitionLayout = 1ULL << 6,
    kCapabilityExceptionLayout = 1ULL << 7,
    kCapabilityTypeArgumentsLayout = 1ULL << 8,
    kCapabilityArtifactLifecycle = 1ULL << 9,
};

struct ArtifactIncarnation {
    std::string name;
    std::string path;
    std::string build_id;
    uintptr_t load_bias = 0;
    std::vector<ExecutableRange> executable_ranges;
};

struct ArtifactSet {
    ArtifactIncarnation app;
    std::vector<ArtifactIncarnation> engines;
};

struct CandidateDiagnostic {
    const RuntimeProfileRecord* profile = nullptr;
    bool snapshot_hash_match = false;
    CandidateProbe probe{};
};

struct ResolverInput {
    VmRuntimeFacts facts{};
    uint64_t thread = 0;
    uint64_t current_isolate = 0;
    uint64_t canonical_null = 0;
    RegisterEvidence registers{};
    const std::vector<ModuleImage>* modules = nullptr;
    // Optional executable address owned by the concrete VM/engine instance
    // that supplied the runtime API table. When present, artifact lifecycle
    // binding is scoped to the unique libflutter.so containing this address
    // instead of every Flutter engine currently mapped in the process.
    uintptr_t engine_anchor = 0;
    uint32_t only_profile_version = 0;
};

struct VerifiedBinding {
    const RuntimeProfileRecord* profile = nullptr;
    CandidateProbe probe{};
    uint64_t capabilities = kCapabilityNone;
    ArtifactSet artifacts{};
};

struct ResolverResult {
    std::vector<CandidateDiagnostic> candidates;
    VerifiedBinding binding{};
    size_t passed_rows = 0;
    size_t distinct_abis = 0;
    bool passed = false;
};

struct CandidateSelection {
    const CandidateDiagnostic* selected = nullptr;
    size_t passed_rows = 0;
    size_t distinct_abis = 0;
};

CandidateSelection SelectUniquePassingCandidate(const std::vector<CandidateDiagnostic>& candidates);

ResolverResult ResolveVerifiedBinding(const ResolverInput& input);

// Binds an API-DL executable anchor to exactly one mapped Flutter engine.
// Multiple matches are ambiguous and fail closed.
bool ResolveEngineIncarnationForAnchor(const std::vector<ModuleImage>& modules, uintptr_t anchor,
                                       ArtifactIncarnation* out_incarnation);

bool ArtifactIncarnationMatches(const ArtifactIncarnation& expected, const ModuleImage& current);
bool ValidateArtifactSet(const ArtifactSet& expected,
                         const std::vector<ModuleImage>& current_modules);

}  // namespace dartplant::vm_abi

#endif  // DARTPLANT_VM_ABI_RESOLVER_H_
