// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_VM_ABI_RESOLVER_H_
#define DARTPLANT_VM_ABI_RESOLVER_H_

#include <cstdint>
#include <span>
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
    kCapabilityExceptionBridgeLayout = 1ULL << 16,
    kCapabilityDeferredLoadingUnitLayout = 1ULL << 17,
    kCapabilityLiveFunctionIndexLayout = 1ULL << 18,
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
    uint64_t isolate_group = 0;
    uintptr_t engine_load_bias = 0;
};

struct CapabilityDescriptor {
    uint64_t capability = kCapabilityNone;
    const char* key = nullptr;
    const char* diagnostic_name = nullptr;
    bool cold_required = false;
    bool verified_after_create = false;
    bool ci_required_event = false;
};

AbiDomainMask CapabilityDomains(uint64_t capability);
std::string BuildCapabilityAbiKey(const RuntimeProfileRecord& profile, uint64_t capability);
const CapabilityDescriptor* CapabilityRegistry();
size_t CapabilityRegistrySize();
const CapabilityDescriptor* FindCapabilityDescriptor(uint64_t capability);
uint64_t ColdRequiredCapabilityMask();
uint64_t VerifiedAfterCreateCapabilityMask();
uint64_t ProfileAbiSelectableCapabilityMask(uint64_t capabilities);

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

uint64_t CandidateCapabilities(const RuntimeProfileRecord& profile, const CandidateProbe& probe,
                               const RegisterEvidence& registers, const ArtifactSet& artifacts);

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

struct DomainSetSelection;

struct CapabilityOwnerStamp {
    uint64_t runtime_generation = 0;
    uint64_t engine_incarnation_epoch = 0;
    uint64_t isolate_group_incarnation_epoch = 0;

    bool valid() const {
        return runtime_generation != 0 && engine_incarnation_epoch != 0 &&
               isolate_group_incarnation_epoch != 0;
    }
    bool operator==(const CapabilityOwnerStamp&) const = default;
};

// One independently selected capability domain. A representative is only a
// source row for fields owned by this capability; it must never be treated as
// the process-wide/current VM profile. selected_rows retains every source row
// carrying the same unique capability fingerprint so downstream consumers can
// explicitly intersect multiple capabilities when they truly require one
// coherent source row.
struct CapabilityBinding {
    uint64_t capability = kCapabilityNone;
    const RuntimeProfileRecord* representative = nullptr;
    std::string abi_domain_key;
    std::vector<const RuntimeProfileRecord*> selected_rows;
    CapabilityOwnerStamp owner{};

    bool bound() const { return capability != kCapabilityNone && representative != nullptr; }
    bool bound_for(const CapabilityOwnerStamp& expected) const {
        return bound() && owner.valid() && owner == expected;
    }
    bool Contains(const RuntimeProfileRecord* profile) const;
};

struct CapabilityBindingSet {
    std::vector<CapabilityBinding> bindings;
    CapabilityOwnerStamp owner{};

    const CapabilityBinding* Find(uint64_t capability) const;
    const CapabilityBinding* FindForOwner(uint64_t capability,
                                          const CapabilityOwnerStamp& expected) const;
    bool Bind(uint64_t capability, const DomainSetSelection& selection);
    void StampOwner(const CapabilityOwnerStamp& new_owner);
    void Clear(uint64_t capability);
};

struct VerifiedBinding {
    VerifiedCoreBinding core{};
    CapabilityBindingSet capability_bindings{};
    uint64_t capability_mask = kCapabilityNone;
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
    std::vector<const RuntimeProfileRecord*> selected_rows;

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

// Returns a deterministic source row only when every requested capability is
// bound and at least one source row belongs to all selected-row sets. This is
// the only supported way to obtain a whole RuntimeProfileRecord for a legacy
// helper that genuinely consumes fields from multiple capabilities.
const RuntimeProfileRecord* ResolveCapabilityConsumerProfile(
    const CapabilityBindingSet& bindings, std::span<const uint64_t> capabilities);

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
