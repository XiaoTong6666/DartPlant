// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "vm/abi/resolver.h"

#include <algorithm>
#include <set>
#include <string_view>

namespace dartplant::vm_abi {
namespace {

ArtifactIncarnation CaptureIncarnation(const ModuleImage& module) {
    return {
        .name = module.name,
        .path = module.path,
        .build_id = module.build_id,
        .load_bias = module.load_bias,
        .executable_range_count = module.executable_ranges.size(),
    };
}

uint64_t CapabilitiesFor(const CandidateProbe& probe, const RegisterEvidence& registers,
                         const ArtifactSet& artifacts) {
    if (!probe.passed) return kCapabilityNone;
    uint64_t capabilities = kCapabilityRuntimeRoots | kCapabilitySafepointStubs |
                            kCapabilityGeneratedTransitionLayout | kCapabilityExceptionLayout |
                            kCapabilityTypeArgumentsLayout;
    if (probe.roots.owner_match) capabilities |= kCapabilityOwnerIdentity;
    if (probe.roots.canonical_null_match) capabilities |= kCapabilityCanonicalNull;
    if (probe.roots.dart_core_found) capabilities |= kCapabilityDartCore;
    if (registers.available && probe.roots.register_semantics_match) {
        capabilities |= kCapabilityRegisterSemantics;
    }
    if (!artifacts.app.path.empty()) capabilities |= kCapabilityArtifactLifecycle;
    return capabilities;
}

}  // namespace

ResolverResult ResolveVerifiedBinding(const ResolverInput& input) {
    ResolverResult result{};
    if (input.thread == 0 || input.modules == nullptr) return result;

    const auto profiles = ResolveRuntimeProfileCandidates(input.facts);
    std::set<std::string_view> passed_abis;
    for (const RuntimeProfileRecord* profile : profiles) {
        if (profile == nullptr ||
            (input.only_profile_version != 0 &&
             profile->live_vm.profile_version != input.only_profile_version)) {
            continue;
        }
        CandidateDiagnostic diagnostic{};
        diagnostic.profile = profile;
        diagnostic.snapshot_hash_match =
            !input.facts.snapshot_hash.empty() &&
            input.facts.snapshot_hash == profile->live_vm.snapshot_hash;
        CandidateProbeInput probe_input{};
        probe_input.roots.profile = profile;
        probe_input.roots.thread = input.thread;
        probe_input.roots.current_isolate = input.current_isolate;
        probe_input.roots.canonical_null = input.canonical_null;
        probe_input.roots.require_current_isolate = input.current_isolate != 0;
        probe_input.roots.require_canonical_null = input.canonical_null != 0;
        probe_input.roots.require_dart_core = true;
        probe_input.roots.registers = input.registers;
        probe_input.modules = input.modules;
        diagnostic.probe = ProbeCandidate(probe_input);
        if (diagnostic.probe.passed) {
            ++result.passed_rows;
            passed_abis.insert(profile->abi_id);
        }
        result.candidates.push_back(std::move(diagnostic));
    }
    result.distinct_abis = passed_abis.size();
    if (result.distinct_abis != 1) return result;

    const std::string_view selected_abi = *passed_abis.begin();
    const CandidateDiagnostic* selected = nullptr;
    for (const CandidateDiagnostic& candidate : result.candidates) {
        if (!candidate.probe.passed || candidate.profile == nullptr ||
            candidate.profile->abi_id != selected_abi) {
            continue;
        }
        if (selected == nullptr ||
            (!selected->snapshot_hash_match && candidate.snapshot_hash_match)) {
            selected = &candidate;
        }
    }
    if (selected == nullptr || selected->probe.code_module == nullptr) return result;

    result.binding.profile = selected->profile;
    result.binding.probe = selected->probe;
    result.binding.artifacts.app = CaptureIncarnation(*selected->probe.code_module);
    for (const ModuleImage& module : *input.modules) {
        if (module.name == "libflutter.so") {
            result.binding.artifacts.engines.push_back(CaptureIncarnation(module));
        }
    }
    result.binding.capabilities =
        CapabilitiesFor(result.binding.probe, input.registers, result.binding.artifacts);
    result.passed = true;
    return result;
}

bool ArtifactIncarnationMatches(const ArtifactIncarnation& expected, const ModuleImage& current) {
    if (expected.name != current.name || expected.path != current.path ||
        expected.load_bias != current.load_bias ||
        expected.executable_range_count != current.executable_ranges.size()) {
        return false;
    }
    return expected.build_id.empty() || current.build_id.empty() ||
           expected.build_id == current.build_id;
}

bool ValidateArtifactSet(const ArtifactSet& expected,
                         const std::vector<ModuleImage>& current_modules) {
    const auto app = std::find_if(current_modules.begin(), current_modules.end(),
                                  [&expected](const ModuleImage& module) {
                                      return ArtifactIncarnationMatches(expected.app, module);
                                  });
    if (app == current_modules.end()) return false;

    size_t matched_engines = 0;
    for (const ArtifactIncarnation& engine : expected.engines) {
        const auto found = std::find_if(current_modules.begin(), current_modules.end(),
                                        [&engine](const ModuleImage& module) {
                                            return ArtifactIncarnationMatches(engine, module);
                                        });
        if (found == current_modules.end()) return false;
        ++matched_engines;
    }
    const size_t current_engine_count = static_cast<size_t>(
        std::count_if(current_modules.begin(), current_modules.end(),
                      [](const ModuleImage& module) { return module.name == "libflutter.so"; }));
    return matched_engines == expected.engines.size() &&
           current_engine_count == expected.engines.size();
}

}  // namespace dartplant::vm_abi
