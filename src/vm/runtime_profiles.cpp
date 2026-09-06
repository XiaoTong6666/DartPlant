// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "vm/runtime_profiles.h"

#include <algorithm>
#include <iterator>

#include "vm/generated/runtime_profiles.generated.h"

namespace dartplant {
namespace {

bool HasSnapshotFeature(std::string_view features, std::string_view expected) {
    while (!features.empty()) {
        const size_t separator = features.find(' ');
        const std::string_view token = features.substr(0, separator);
        if (token == expected) return true;
        if (separator == std::string_view::npos) break;
        features.remove_prefix(separator + 1);
    }
    return false;
}

bool MachineFactsMatch(const RuntimeProfileRecord& profile, const VmRuntimeFacts& facts) {
    if (profile.machine.architecture != facts.architecture ||
        profile.machine.pointer_size != facts.pointer_size ||
        profile.machine.product != facts.product ||
        profile.machine.compressed_pointers != facts.compressed_pointers) {
        return false;
    }
    // snapshot_features is independent runtime evidence when available. Do
    // not require it merely to enumerate a candidate: callers may only know
    // machine facts during early discovery.
    if (!facts.snapshot_features.empty()) {
        if (profile.machine.product && !HasSnapshotFeature(facts.snapshot_features, "product")) {
            return false;
        }
        if (profile.machine.compressed_pointers &&
            !HasSnapshotFeature(facts.snapshot_features, "compressed-pointers")) {
            return false;
        }
    }
    return true;
}

}  // namespace

const RuntimeProfileRecord* RuntimeProfiles() { return kGeneratedRuntimeProfiles; }

size_t RuntimeProfileCount() { return std::size(kGeneratedRuntimeProfiles); }

const RuntimeProfileRecord* FindRuntimeProfileByVersion(uint32_t profile_version) {
    const auto* begin = std::begin(kGeneratedRuntimeProfiles);
    const auto* end = std::end(kGeneratedRuntimeProfiles);
    const auto* found =
        std::find_if(begin, end, [profile_version](const RuntimeProfileRecord& item) {
            return item.live_vm.profile_version == profile_version;
        });
    return found == end ? nullptr : found;
}

const RuntimeProfileRecord* FindRuntimeProfileBySnapshot(std::string_view snapshot_hash,
                                                         std::string_view snapshot_profile) {
    const auto* begin = std::begin(kGeneratedRuntimeProfiles);
    const auto* end = std::end(kGeneratedRuntimeProfiles);
    const auto* found = std::find_if(begin, end, [&](const RuntimeProfileRecord& item) {
        const std::string_view hash =
            item.live_vm.snapshot_hash == nullptr ? std::string_view{} : item.live_vm.snapshot_hash;
        const std::string_view profile = item.live_vm.snapshot_profile == nullptr
                                             ? std::string_view{}
                                             : item.live_vm.snapshot_profile;
        return hash == snapshot_hash && (snapshot_profile.empty() || profile == snapshot_profile);
    });
    return found == end ? nullptr : found;
}

std::vector<const RuntimeProfileRecord*> ResolveRuntimeProfileCandidates(
    const VmRuntimeFacts& facts) {
    std::vector<const RuntimeProfileRecord*> compatible;
    compatible.reserve(RuntimeProfileCount());

    for (size_t index = 0; index < RuntimeProfileCount(); ++index) {
        const RuntimeProfileRecord& profile = RuntimeProfiles()[index];
        if (!MachineFactsMatch(profile, facts)) continue;
        compatible.push_back(&profile);
    }

    // Snapshot identity is a ranking hint, never an ABI gate. Probe exact
    // aliases first for deterministic diagnostics, then every other finite,
    // source-verified machine/mode candidate. This also exercises the same
    // fail-closed path for custom/rebuilt engines whose snapshot identity is
    // unknown while their private ABI is already in the registry.
    std::stable_sort(compatible.begin(), compatible.end(),
                     [&](const RuntimeProfileRecord* left, const RuntimeProfileRecord* right) {
                         const auto exact = [&](const RuntimeProfileRecord* profile) {
                             return !facts.snapshot_hash.empty() &&
                                    profile->live_vm.snapshot_hash != nullptr &&
                                    facts.snapshot_hash == profile->live_vm.snapshot_hash;
                         };
                         return exact(left) && !exact(right);
                     });
    return compatible;
}

uint32_t ThreadJumpToFrameOffsetForSnapshot(std::string_view snapshot_hash) {
    const RuntimeProfileRecord* profile = FindRuntimeProfileBySnapshot(snapshot_hash);
    return profile == nullptr ? 0 : profile->thread_jump_to_frame_entry_point_offset;
}

bool IsClosureFunctionKind(uint32_t profile_version, uint32_t function_kind) {
    const RuntimeProfileRecord* profile = FindRuntimeProfileByVersion(profile_version);
    return profile != nullptr && (function_kind == profile->function_kind.closure ||
                                  function_kind == profile->function_kind.implicit_closure);
}

bool ComputeAotCodePayloadStart(uint32_t profile_version, uint64_t normal_entry,
                                uint64_t monomorphic_entry, uint64_t* out_start,
                                bool* out_has_monomorphic_entry) {
    if (out_start == nullptr || normal_entry == 0 || monomorphic_entry == 0) {
        return false;
    }
    const RuntimeProfileRecord* profile = FindRuntimeProfileByVersion(profile_version);
    if (profile == nullptr) return false;

    const bool has_monomorphic_entry = normal_entry != monomorphic_entry;
    const uint64_t normal_offset =
        has_monomorphic_entry ? profile->instructions_polymorphic_entry_offset_aot : 0;
    const uint64_t monomorphic_offset =
        has_monomorphic_entry ? profile->instructions_monomorphic_entry_offset_aot : 0;
    if (normal_entry < normal_offset || monomorphic_entry < monomorphic_offset) return false;
    const uint64_t start = normal_entry - normal_offset;
    if (start > UINT64_MAX - monomorphic_offset ||
        monomorphic_entry != start + monomorphic_offset) {
        return false;
    }
    *out_start = start;
    if (out_has_monomorphic_entry != nullptr) {
        *out_has_monomorphic_entry = has_monomorphic_entry;
    }
    return true;
}

bool ComputeAotCodePayloadRange(uint32_t profile_version, uint64_t normal_entry,
                                uint64_t monomorphic_entry, uint32_t instructions_length,
                                AotCodePayloadRange* out_range) {
    if (out_range == nullptr || instructions_length == 0) return false;
    uint64_t start = 0;
    bool has_monomorphic_entry = false;
    if (!ComputeAotCodePayloadStart(profile_version, normal_entry, monomorphic_entry, &start,
                                    &has_monomorphic_entry) ||
        start > UINT64_MAX - instructions_length) {
        return false;
    }
    const uint64_t end = start + instructions_length;
    if (normal_entry < start || normal_entry >= end || monomorphic_entry < start ||
        monomorphic_entry >= end) {
        return false;
    }
    *out_range = {
        .start = start,
        .end = end,
        .has_monomorphic_entry = has_monomorphic_entry,
    };
    return true;
}

}  // namespace dartplant
