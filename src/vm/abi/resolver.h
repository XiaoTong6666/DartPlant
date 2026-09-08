// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_VM_ABI_RESOLVER_H_
#define DARTPLANT_VM_ABI_RESOLVER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "vm/abi/candidate_set.h"
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
    kCapabilityArgumentsDescriptorLayout = 1ULL << 10,
    kCapabilityInvocationCallAbi = 1ULL << 11,
    kCapabilityFunctionCodeLayout = 1ULL << 12,
    kCapabilityAotEntryLayout = 1ULL << 13,
    kCapabilityFunctionTypeLayout = 1ULL << 14,
    kCapabilityClosureCallLayout = 1ULL << 15,
};

enum class ProofState : uint8_t {
    kUnavailable = 0,
    kUnverified,
    kVerified,
    kFailedForIncarnation,
    kAmbiguous,
};

struct CapabilityProofRecord {
    uint64_t capability = kCapabilityNone;
    ProofState state = ProofState::kUnavailable;
    std::string abi_domain_key;
    AbiDomainMask abi_domains = 0;
    uint64_t artifact_generation = 0;
    uint64_t isolate_generation = 0;
};

AbiDomainMask CapabilityDomains(uint64_t capability);
std::string BuildCapabilityAbiKey(const RuntimeProfileRecord& profile, uint64_t capability);

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

struct VerifiedCoreBinding {
    AbiCandidateSet candidates{};
    const RuntimeProfileRecord* representative = nullptr;
    std::string_view core_abi_id;
    CandidateProbe core_probe{};
    uint64_t capabilities = kCapabilityNone;
};

struct VerifiedBinding {
    VerifiedCoreBinding core{};
    // Compatibility projection. New private-layout consumers must resolve the
    // owning domain from core.candidates rather than trust this whole row.
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
CandidateSelection SelectUniquePassingCandidate(const std::vector<CandidateDiagnostic>& candidates,
                                                AbiDomain domain);

struct DomainSetSelection {
    const RuntimeProfileRecord* representative = nullptr;
    std::string abi_domain_key;
    size_t compatible_rows = 0;
    size_t distinct_domain_sets = 0;

    bool passed() const { return representative != nullptr && distinct_domain_sets == 1; }
    bool ambiguous() const { return distinct_domain_sets > 1; }
};

DomainSetSelection SelectDomainAbiSet(const AbiCandidateSet& candidates,
                                      const std::vector<AbiDomain>& domains,
                                      const std::vector<bool>& compatible);
DomainSetSelection SelectDomainAbiSet(const AbiCandidateSet& candidates, AbiDomainMask domains,
                                      const std::vector<bool>& compatible);
DomainSetSelection SelectCapabilityAbiSet(const AbiCandidateSet& candidates, uint64_t capability,
                                          const std::vector<bool>& compatible);

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
