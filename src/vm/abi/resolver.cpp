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
        .executable_ranges = module.executable_ranges,
    };
}

uint64_t CapabilitiesFor(const RuntimeProfileRecord& profile, const CandidateProbe& probe,
                         const RegisterEvidence& registers, const ArtifactSet& artifacts) {
    if (!probe.passed) return kCapabilityNone;
    uint64_t capabilities = kCapabilityRuntimeRoots | kCapabilitySafepointStubs;
    if (probe.roots.owner_match) capabilities |= kCapabilityOwnerIdentity;
    if (probe.roots.canonical_null_match) capabilities |= kCapabilityCanonicalNull;
    if (probe.roots.dart_core_found) capabilities |= kCapabilityDartCore;
    if (registers.available && probe.roots.register_semantics_match) {
        capabilities |= kCapabilityRegisterSemantics;
    }
    if (!artifacts.app.path.empty() && !artifacts.engines.empty()) {
        capabilities |= kCapabilityArtifactLifecycle;
    }
    const VmThreadBridgeLayout& bridge = profile.thread_bridge;
    if (bridge.top_exit_frame_offset != 0 && bridge.vm_tag_offset != 0 &&
        bridge.execution_state_offset != 0 && bridge.exit_through_ffi_offset != 0 &&
        profile.transition.vm_tag_dart != 0) {
        capabilities |= kCapabilityGeneratedTransitionLayout;
    }
    if (bridge.active_exception_offset != 0 && bridge.active_stacktrace_offset != 0) {
        capabilities |= kCapabilityExceptionLayout;
    }
    if (profile.type_arguments.cid != 0 && profile.type_arguments.length_offset != 0 &&
        profile.type_arguments.types_offset != 0) {
        capabilities |= kCapabilityTypeArgumentsLayout;
    }
    return capabilities;
}

}  // namespace

CandidateSelection SelectUniquePassingCandidate(
    const std::vector<CandidateDiagnostic>& candidates) {
    CandidateSelection selection{};
    std::set<std::string_view> passed_abis;
    for (const CandidateDiagnostic& candidate : candidates) {
        if (!candidate.probe.passed || candidate.profile == nullptr ||
            candidate.profile->abi_id == nullptr) {
            continue;
        }
        ++selection.passed_rows;
        passed_abis.insert(candidate.profile->abi_id);
    }
    selection.distinct_abis = passed_abis.size();
    if (selection.distinct_abis != 1) return selection;

    const std::string_view selected_abi = *passed_abis.begin();
    for (const CandidateDiagnostic& candidate : candidates) {
        if (!candidate.probe.passed || candidate.profile == nullptr ||
            candidate.profile->abi_id != selected_abi) {
            continue;
        }
        if (selection.selected == nullptr ||
            (!selection.selected->snapshot_hash_match && candidate.snapshot_hash_match)) {
            selection.selected = &candidate;
        }
    }
    return selection;
}

bool ResolveEngineIncarnationForAnchor(const std::vector<ModuleImage>& modules, uintptr_t anchor,
                                       ArtifactIncarnation* out_incarnation) {
    if (anchor == 0 || out_incarnation == nullptr) return false;
    const ModuleImage* matched = nullptr;
    for (const ModuleImage& module : modules) {
        if (module.name != "libflutter.so" ||
            !module.ContainsExecutable(anchor, sizeof(uint32_t))) {
            continue;
        }
        if (matched != nullptr) return false;
        matched = &module;
    }
    if (matched == nullptr) return false;
    *out_incarnation = CaptureIncarnation(*matched);
    return true;
}

ResolverResult ResolveVerifiedBinding(const ResolverInput& input) {
    ResolverResult result{};
    if (input.thread == 0 || input.modules == nullptr) return result;

    const auto profiles = ResolveRuntimeProfileCandidates(input.facts);
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
        result.candidates.push_back(std::move(diagnostic));
    }
    const CandidateSelection selection = SelectUniquePassingCandidate(result.candidates);
    result.passed_rows = selection.passed_rows;
    result.distinct_abis = selection.distinct_abis;
    const CandidateDiagnostic* selected = selection.selected;
    if (selected == nullptr || selected->probe.code_module == nullptr) return result;

    result.binding.profile = selected->profile;
    result.binding.probe = selected->probe;
    result.binding.artifacts.app = CaptureIncarnation(*selected->probe.code_module);
    if (input.engine_anchor != 0) {
        ArtifactIncarnation engine{};
        if (ResolveEngineIncarnationForAnchor(*input.modules, input.engine_anchor, &engine)) {
            result.binding.artifacts.engines.push_back(std::move(engine));
        }
    } else {
        for (const ModuleImage& module : *input.modules) {
            if (module.name == "libflutter.so") {
                result.binding.artifacts.engines.push_back(CaptureIncarnation(module));
            }
        }
    }
    result.binding.capabilities = CapabilitiesFor(*result.binding.profile, result.binding.probe,
                                                  input.registers, result.binding.artifacts);
    result.passed = true;
    return result;
}

bool ArtifactIncarnationMatches(const ArtifactIncarnation& expected, const ModuleImage& current) {
    if (expected.name != current.name || expected.path != current.path ||
        expected.load_bias != current.load_bias ||
        expected.executable_ranges.size() != current.executable_ranges.size()) {
        return false;
    }
    if (expected.build_id != current.build_id) return false;
    for (size_t index = 0; index < expected.executable_ranges.size(); ++index) {
        const ExecutableRange& left = expected.executable_ranges[index];
        const ExecutableRange& right = current.executable_ranges[index];
        if (left.start != right.start || left.end != right.end ||
            left.file_offset != right.file_offset ||
            left.virtual_address != right.virtual_address || left.file_size != right.file_size) {
            return false;
        }
    }
    return true;
}

bool ValidateArtifactSet(const ArtifactSet& expected,
                         const std::vector<ModuleImage>& current_modules) {
    const auto app = std::find_if(current_modules.begin(), current_modules.end(),
                                  [&expected](const ModuleImage& module) {
                                      return ArtifactIncarnationMatches(expected.app, module);
                                  });
    if (app == current_modules.end()) return false;

    std::vector<bool> used(current_modules.size(), false);
    size_t matched_engines = 0;
    for (const ArtifactIncarnation& engine : expected.engines) {
        size_t found_index = current_modules.size();
        for (size_t index = 0; index < current_modules.size(); ++index) {
            if (used[index] || !ArtifactIncarnationMatches(engine, current_modules[index])) {
                continue;
            }
            found_index = index;
            break;
        }
        if (found_index == current_modules.size()) return false;
        used[found_index] = true;
        ++matched_engines;
    }
    return matched_engines == expected.engines.size();
}

}  // namespace dartplant::vm_abi
