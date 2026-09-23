#include "runtime/snapshot_index.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include "core/internal.h"
#include "core/method_model.h"
#include "vm/abi/resolver.h"
#include "vm/live_vm_internal.h"
#include "vm/runtime_profiles.h"

namespace dartplant {
namespace {

uint64_t RuntimeEntryForKind(const DartPlantLiveVmFunctionInfo& function, DartPlantEntryKind kind) {
    switch (kind) {
    case DARTPLANT_ENTRY_DEFAULT:
        return function.code_entry_point;
    case DARTPLANT_ENTRY_UNCHECKED:
        return function.code_unchecked_entry_point;
    case DARTPLANT_ENTRY_MONOMORPHIC:
        return function.code_monomorphic_entry_point;
    case DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED:
        return function.code_monomorphic_unchecked_entry_point;
    }
    return 0;
}

uint64_t EntryVaForKind(const DartPlantLiveVmFunctionInfo& function, DartPlantEntryKind kind) {
    switch (kind) {
    case DARTPLANT_ENTRY_DEFAULT:
        return function.entry_va;
    case DARTPLANT_ENTRY_UNCHECKED:
        return function.unchecked_entry_va;
    case DARTPLANT_ENTRY_MONOMORPHIC:
        return function.monomorphic_entry_va;
    case DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED:
        return function.monomorphic_unchecked_entry_va;
    }
    return 0;
}

bool AppendLiveSnapshotRecord(const LiveVmFunctionSnapshotRecord& record,
                              const RuntimeProfileRecord& live_index_profile,
                              const RuntimeProfileRecord& function_type_profile,
                              SnapshotIndex* index) {
    if (index == nullptr ||
        !AppendLiveSnapshotFunctionRecord(record.function,
                                          live_index_profile.live_vm.profile_version, index)) {
        return false;
    }
    if (!record.has_semantics) return true;
    index->live_function_semantics.push_back({
        .runtime_image_id = record.function.runtime_image_id,
        .runtime_image_incarnation_epoch = record.function.runtime_image_incarnation_epoch,
        .engine_incarnation_epoch = record.function.engine_incarnation_epoch,
        .isolate_group_incarnation_epoch = record.function.isolate_group_incarnation_epoch,
        .runtime_generation = record.function.runtime_generation,
        .owner_class_id = record.function.owner_class_id,
        .owner_function_index = record.function.owner_function_index,
        .library_uri = record.function.library_uri,
        .class_name = record.function.class_name,
        .function_name = record.function.function_name,
        .function_type_profile_version = function_type_profile.live_vm.profile_version,
        .function_type_abi_key = vm_abi::BuildCapabilityAbiKey(
            function_type_profile, vm_abi::kCapabilityFunctionTypeLayout),
        .signature = record.signature,
        .parameters = record.parameters,
    });
    return true;
}

bool HasStableOwnerSlot(const DartPlantLiveVmFunctionInfo& function) {
    return function.owner_class_id != 0 && function.owner_function_index != UINT32_MAX;
}

uint64_t StableOwnerSlotKey(uint32_t owner_class_id, uint32_t owner_function_index) {
    return (static_cast<uint64_t>(owner_class_id) << 32) | owner_function_index;
}

uint64_t StableOwnerSlotKey(const DartPlantLiveVmFunctionInfo& function) {
    return StableOwnerSlotKey(function.owner_class_id, function.owner_function_index);
}

bool RecomputeLiveDirectoryAliasMetadata(SnapshotIndex* index, uint32_t skipped_function_count,
                                         DartPlantLiveVmFunctionIndexInfo* out_info) {
    if (index == nullptr || out_info == nullptr ||
        index->live_function_infos.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    std::array<std::unordered_map<uint64_t, uint32_t>, 4> aliases_by_kind;
    for (const auto& function : index->live_function_infos) {
        if (function.entry_kind_mask == 0) return false;
        for (uint32_t kind = 0; kind < 4; ++kind) {
            if ((function.entry_kind_mask & (1u << kind)) == 0) continue;
            const uint64_t entry =
                RuntimeEntryForKind(function, static_cast<DartPlantEntryKind>(kind));
            if (entry == 0) return false;
            uint32_t& count = aliases_by_kind[kind][entry];
            if (count != UINT32_MAX) ++count;
        }
    }
    for (auto& function : index->live_function_infos) {
        for (uint32_t kind = 0; kind < 4; ++kind) {
            if ((function.entry_kind_mask & (1u << kind)) == 0) {
                function.entry_alias_counts[kind] = 0;
                continue;
            }
            const uint64_t entry =
                RuntimeEntryForKind(function, static_cast<DartPlantEntryKind>(kind));
            const auto found = aliases_by_kind[kind].find(entry);
            if (found == aliases_by_kind[kind].end() || found->second == 0) return false;
            function.entry_alias_counts[kind] = found->second;
        }
        function.entry_alias_count = function.entry_alias_counts[DARTPLANT_ENTRY_DEFAULT];
        function.entry_is_shared = function.entry_alias_count > 1 ? 1 : 0;
    }
    for (auto& function : index->functions) {
        if (!function.live) continue;
        const uint32_t kind = static_cast<uint32_t>(function.entry_kind);
        if (kind >= aliases_by_kind.size() || function.runtime_entry == 0) return false;
        const auto found = aliases_by_kind[kind].find(function.runtime_entry);
        if (found == aliases_by_kind[kind].end() || found->second == 0) return false;
        function.entry_alias_count = found->second;
    }

    uint32_t shared_targets = 0;
    for (const auto& [entry, count] : aliases_by_kind[DARTPLANT_ENTRY_DEFAULT]) {
        (void) entry;
        if (count > 1 && shared_targets != UINT32_MAX) ++shared_targets;
    }
    DartPlantLiveVmFunctionIndexInfo info{};
    info.struct_size = sizeof(info);
    info.function_count = static_cast<uint32_t>(index->live_function_infos.size());
    info.code_target_count = static_cast<uint32_t>(aliases_by_kind[DARTPLANT_ENTRY_DEFAULT].size());
    info.shared_code_target_count = shared_targets;
    info.skipped_function_count = skipped_function_count;
    *out_info = info;
    return true;
}

}  // namespace

bool AppendLiveSnapshotFunctionRecord(const DartPlantLiveVmFunctionInfo& function,
                                      uint32_t profile_version, SnapshotIndex* index) {
    if (index == nullptr || function.entry_kind_mask == 0 || function.code_size == 0) return false;
    AotCodePayloadRange payload_range{};
    if (!ComputeAotCodePayloadRange(profile_version, function.code_entry_point,
                                    function.code_monomorphic_entry_point, function.code_size,
                                    &payload_range)) {
        return false;
    }
    for (uint32_t raw_kind = 0; raw_kind < 4; ++raw_kind) {
        if ((function.entry_kind_mask & (1u << raw_kind)) == 0) continue;
        const auto kind = static_cast<DartPlantEntryKind>(raw_kind);
        const uint64_t runtime_entry = RuntimeEntryForKind(function, kind);
        if (runtime_entry == 0 || EntryVaForKind(function, kind) == 0) return false;
        if (runtime_entry < payload_range.start || runtime_entry >= payload_range.end) return false;
    }
    for (uint32_t raw_kind = 0; raw_kind < 4; ++raw_kind) {
        if ((function.entry_kind_mask & (1u << raw_kind)) == 0) continue;
        const auto kind = static_cast<DartPlantEntryKind>(raw_kind);
        const uint64_t runtime_entry = RuntimeEntryForKind(function, kind);
        const uint64_t entry_va = EntryVaForKind(function, kind);
        index->functions.push_back({
            .runtime_image_id = function.runtime_image_id,
            .runtime_image_incarnation_epoch = function.runtime_image_incarnation_epoch,
            .engine_incarnation_epoch = function.engine_incarnation_epoch,
            .isolate_group_incarnation_epoch = function.isolate_group_incarnation_epoch,
            .runtime_generation = function.runtime_generation,
            .loading_unit_id = function.loading_unit_id,
            .owner_class_id = function.owner_class_id,
            .owner_function_index = function.owner_function_index,
            .library_uri = function.library_uri,
            .class_name = function.class_name,
            .function_name = function.function_name,
            .signature = "",
            .entry_kind = kind,
            .entry_va = entry_va,
            .code_size = payload_range.end - runtime_entry,
            .code_section_va = function.code_section_va,
            .fingerprint = "",
            .function_object = function.function,
            .code_object = function.code,
            .code_object_pool = function.code_object_pool,
            .code_payload_start = payload_range.start,
            .code_instructions_length = function.code_size,
            .runtime_entry = static_cast<uintptr_t>(runtime_entry),
            .code_entry = static_cast<uintptr_t>(runtime_entry),
            .owner_class = function.owner_class,
            .library = function.library,
            .entry_alias_count = function.entry_alias_counts[raw_kind],
            .function_kind = function.function_kind,
            .closure_call_entry_only = function.closure_call_entry_only != 0,
            .owner_is_toplevel_class = function.owner_is_toplevel_class != 0,
            .code_owner_matches_function = function.code_owner_matches_function != 0,
            .live = true,
        });
    }
    index->live_function_infos.push_back(function);
    return true;
}

bool BindLiveSnapshotImageSemantics(const SnapshotIndex& index,
                                    const RuntimeImageSet& current_images,
                                    RuntimeImageSet* out_images, std::string* error) {
    if (out_images == nullptr) {
        if (error != nullptr) *error = "live image semantic output is null";
        return false;
    }

    struct FamilyState {
        RuntimeImageId image_id = kInvalidRuntimeImageId;
        RuntimeImageIncarnationEpoch image_incarnation_epoch = 0;
        uint64_t engine_incarnation_epoch = 0;
        uint64_t isolate_group_incarnation_epoch = 0;
        uint64_t runtime_generation = 0;
        uint32_t loading_unit_id = 0;
        uint64_t code_object = 0;
        uint8_t expected_entry_mask = 0;
        uint8_t observed_entry_mask = 0;
    };

    std::unordered_map<uint64_t, FamilyState> families;
    families.reserve(index.live_function_infos.size());
    std::vector<RuntimeImageId> semantic_bindings;
    semantic_bindings.reserve(index.live_function_infos.size());

    for (const auto& function : index.live_function_infos) {
        const uint8_t entry_mask = function.entry_kind_mask;
        if (function.function == 0 || function.code == 0 || entry_mask == 0 ||
            (entry_mask & ~uint8_t{0x0f}) != 0 ||
            function.runtime_image_id == kInvalidRuntimeImageId) {
            if (error != nullptr)
                *error = "live Function entry family has incomplete image identity";
            return false;
        }
        const RuntimeImage* image = current_images.FindById(function.runtime_image_id);
        if (image == nullptr || function.loading_unit_id != image->loading_unit_id ||
            function.runtime_image_incarnation_epoch != image->incarnation_epoch ||
            function.engine_incarnation_epoch != image->engine_incarnation_epoch ||
            function.isolate_group_incarnation_epoch != image->isolate_group_incarnation_epoch ||
            function.runtime_generation != image->runtime_generation) {
            if (error != nullptr)
                *error = "live Function entry family disagrees with runtime image namespace";
            return false;
        }
        const auto [_, inserted] = families.emplace(
            function.function,
            FamilyState{
                .image_id = function.runtime_image_id,
                .image_incarnation_epoch = function.runtime_image_incarnation_epoch,
                .engine_incarnation_epoch = function.engine_incarnation_epoch,
                .isolate_group_incarnation_epoch = function.isolate_group_incarnation_epoch,
                .runtime_generation = function.runtime_generation,
                .loading_unit_id = function.loading_unit_id,
                .code_object = function.code,
                .expected_entry_mask = entry_mask,
            });
        if (!inserted) {
            if (error != nullptr) *error = "live Function entry family is duplicated";
            return false;
        }
        semantic_bindings.push_back(function.runtime_image_id);
    }

    for (const SnapshotFunction& record : index.functions) {
        if (!record.live || record.function_object == 0 || record.code_object == 0 ||
            record.runtime_image_id == kInvalidRuntimeImageId) {
            if (error != nullptr) *error = "flattened live Function record is incomplete";
            return false;
        }
        const auto family_it = families.find(record.function_object);
        if (family_it == families.end()) {
            if (error != nullptr)
                *error = "flattened live Function record has no canonical entry family";
            return false;
        }
        FamilyState& family = family_it->second;
        if (record.code_object != family.code_object ||
            record.runtime_image_id != family.image_id ||
            record.runtime_image_incarnation_epoch != family.image_incarnation_epoch ||
            record.engine_incarnation_epoch != family.engine_incarnation_epoch ||
            record.isolate_group_incarnation_epoch != family.isolate_group_incarnation_epoch ||
            record.runtime_generation != family.runtime_generation ||
            record.loading_unit_id != family.loading_unit_id) {
            if (error != nullptr)
                *error = "flattened live Function record crosses runtime image identity";
            return false;
        }
        const uint32_t raw_kind = static_cast<uint32_t>(record.entry_kind);
        if (raw_kind >= 4) {
            if (error != nullptr) *error = "flattened live Function record has invalid entry kind";
            return false;
        }
        const uint8_t bit = static_cast<uint8_t>(1u << raw_kind);
        if ((family.expected_entry_mask & bit) == 0 || (family.observed_entry_mask & bit) != 0) {
            if (error != nullptr)
                *error = "flattened live Function record disagrees with entry-family mask";
            return false;
        }
        family.observed_entry_mask |= bit;
    }

    for (const auto& [_, family] : families) {
        if (family.observed_entry_mask != family.expected_entry_mask) {
            if (error != nullptr)
                *error = "live Function entry family was only partially flattened";
            return false;
        }
    }

    RuntimeImageSet rebound = current_images;
    if (!rebound.BindLiveEntries(semantic_bindings)) {
        if (error != nullptr) *error = "live Function index references an unknown runtime image";
        return false;
    }
    *out_images = std::move(rebound);
    return true;
}

bool BindStableLiveSnapshotImageSemantics(const SnapshotIndex& index,
                                          const RuntimeImageSet& current_images,
                                          RuntimeImageSet* out_images, std::string* error) {
    if (out_images == nullptr || !index.stable_live_directory || current_images.empty()) {
        if (error != nullptr) *error = "stable live image semantic input is invalid";
        return false;
    }
    std::vector<RuntimeImageId> semantic_bindings;
    semantic_bindings.reserve(index.live_function_infos.size());
    std::unordered_set<uint64_t> stable_slots;
    stable_slots.reserve(index.live_function_infos.size());
    for (const auto& function : index.live_function_infos) {
        if (function.function != 0 || function.code != 0 || function.code_object_pool != 0 ||
            function.owner_class != 0 || function.library != 0 ||
            function.runtime_image_id == kInvalidRuntimeImageId) {
            if (error != nullptr)
                *error = "stable live Function directory has an invalid retained receipt";
            return false;
        }
        if (HasStableOwnerSlot(function) &&
            !stable_slots.insert(StableOwnerSlotKey(function)).second) {
            if (error != nullptr)
                *error = "stable live Function directory has a duplicate owner-slot receipt";
            return false;
        }
        const RuntimeImage* image = current_images.FindById(function.runtime_image_id);
        if (image == nullptr || !image->IsActive() ||
            function.loading_unit_id != image->loading_unit_id ||
            function.runtime_image_incarnation_epoch != image->incarnation_epoch ||
            function.engine_incarnation_epoch != image->engine_incarnation_epoch ||
            function.isolate_group_incarnation_epoch != image->isolate_group_incarnation_epoch ||
            function.runtime_generation != image->runtime_generation) {
            if (error != nullptr)
                *error = "stable live Function directory crosses runtime image ownership";
            return false;
        }
        const auto contains_entry = [&](uint64_t entry, uint8_t bit) {
            return (function.entry_kind_mask & bit) == 0 ||
                   image->ContainsRuntimeRange(static_cast<uintptr_t>(entry), 4);
        };
        if (!contains_entry(function.code_entry_point, 1u << DARTPLANT_ENTRY_DEFAULT) ||
            !contains_entry(function.code_unchecked_entry_point, 1u << DARTPLANT_ENTRY_UNCHECKED) ||
            !contains_entry(function.code_monomorphic_entry_point,
                            1u << DARTPLANT_ENTRY_MONOMORPHIC) ||
            !contains_entry(function.code_monomorphic_unchecked_entry_point,
                            1u << DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED)) {
            if (error != nullptr)
                *error = "stable live Function directory entry escaped its runtime image";
            return false;
        }
        semantic_bindings.push_back(function.runtime_image_id);
    }
    RuntimeImageSet rebound = current_images;
    if (!rebound.BindLiveEntries(semantic_bindings)) {
        if (error != nullptr)
            *error = "stable live Function directory references an unknown runtime image";
        return false;
    }
    *out_images = std::move(rebound);
    return true;
}

void StabilizeLiveSnapshotIndexForPublication(SnapshotIndex* index) {
    if (index == nullptr) return;
    for (auto& function : index->functions) {
        function.function_object = 0;
        function.code_object = 0;
        function.code_object_pool = 0;
        function.owner_class = 0;
        function.library = 0;
    }
    for (auto& function : index->live_function_infos) {
        function.function = 0;
        function.code = 0;
        function.code_object_pool = 0;
        function.owner_class = 0;
        function.library = 0;
    }
    index->stable_live_directory = true;
}

bool PruneStableLiveSnapshotIndexForImages(SnapshotIndex* index,
                                           std::span<const RuntimeImage> retired_images,
                                           DartPlantLiveVmFunctionIndexInfo* index_info,
                                           std::string* error) {
    if (index == nullptr || index_info == nullptr || retired_images.empty() ||
        !index->stable_live_directory ||
        index_info->struct_size < sizeof(DartPlantLiveVmFunctionIndexInfo) ||
        index_info->function_count != index->live_function_infos.size()) {
        if (error != nullptr) *error = "stable live Function prune input is invalid";
        return false;
    }

    const auto retired_image_index =
        [&retired_images](uint64_t image_id, uint64_t image_epoch, uint64_t engine_epoch,
                          uint64_t group_epoch, uint64_t generation) -> std::optional<size_t> {
        for (size_t i = 0; i < retired_images.size(); ++i) {
            const RuntimeImage& image = retired_images[i];
            if (image.id == image_id && image.incarnation_epoch == image_epoch &&
                image.engine_incarnation_epoch == engine_epoch &&
                image.isolate_group_incarnation_epoch == group_epoch &&
                image.runtime_generation == generation) {
                return i;
            }
        }
        return std::nullopt;
    };

    std::vector<uint32_t> canonical_counts(retired_images.size(), 0);
    for (const auto& function : index->live_function_infos) {
        const auto retired_index = retired_image_index(
            function.runtime_image_id, function.runtime_image_incarnation_epoch,
            function.engine_incarnation_epoch, function.isolate_group_incarnation_epoch,
            function.runtime_generation);
        if (!retired_index.has_value()) continue;
        if (canonical_counts[*retired_index] == UINT32_MAX) {
            if (error != nullptr) *error = "stable live Function prune owner count overflowed";
            return false;
        }
        ++canonical_counts[*retired_index];
    }
    for (size_t i = 0; i < retired_images.size(); ++i) {
        if (canonical_counts[i] != retired_images[i].live_entry_count) {
            if (error != nullptr) {
                *error =
                    "stable live Function prune disagrees with the retired image live-entry count";
            }
            return false;
        }
    }

    const auto retired = [&retired_image_index](uint64_t image_id, uint64_t image_epoch,
                                                uint64_t engine_epoch, uint64_t group_epoch,
                                                uint64_t generation) {
        return retired_image_index(image_id, image_epoch, engine_epoch, group_epoch, generation)
            .has_value();
    };

    SnapshotIndex pruned = *index;
    std::erase_if(pruned.live_function_infos, [&](const DartPlantLiveVmFunctionInfo& function) {
        return retired(function.runtime_image_id, function.runtime_image_incarnation_epoch,
                       function.engine_incarnation_epoch, function.isolate_group_incarnation_epoch,
                       function.runtime_generation);
    });
    std::erase_if(pruned.functions, [&](const SnapshotFunction& function) {
        return retired(function.runtime_image_id, function.runtime_image_incarnation_epoch,
                       function.engine_incarnation_epoch, function.isolate_group_incarnation_epoch,
                       function.runtime_generation);
    });
    std::erase_if(
        pruned.live_function_semantics, [&](const LiveFunctionSemanticSnapshot& semantic) {
            return retired(semantic.runtime_image_id, semantic.runtime_image_incarnation_epoch,
                           semantic.engine_incarnation_epoch,
                           semantic.isolate_group_incarnation_epoch, semantic.runtime_generation);
        });

    if (pruned.live_function_infos.empty() || pruned.functions.empty()) {
        if (error != nullptr) *error = "stable live Function prune removed the entire directory";
        return false;
    }

    DartPlantLiveVmFunctionIndexInfo pruned_info{};
    pruned_info.struct_size = sizeof(pruned_info);
    if (!RecomputeLiveDirectoryAliasMetadata(&pruned, index_info->skipped_function_count,
                                             &pruned_info)) {
        if (error != nullptr) *error = "stable live Function prune produced an invalid directory";
        return false;
    }
    *index = std::move(pruned);
    *index_info = pruned_info;
    return true;
}

bool ValidateReusableLiveSnapshotIndex(const SnapshotIndex& index,
                                       const RuntimeImageSet& current_images,
                                       const RuntimeProfileRecord& live_index_profile,
                                       const RuntimeProfileRecord& function_type_profile,
                                       const DartPlantLiveVmFunctionIndexInfo& index_info,
                                       bool allow_unbound_deferred_additions) {
    if (!index.stable_live_directory || current_images.empty() || index.functions.empty() ||
        index.live_function_infos.empty() ||
        index_info.struct_size < sizeof(DartPlantLiveVmFunctionIndexInfo) ||
        index_info.function_count != index.live_function_infos.size()) {
        return false;
    }
    if (index.vm_profile_version != live_index_profile.live_vm.profile_version ||
        index.live_index_abi_key !=
            vm_abi::BuildCapabilityAbiKey(live_index_profile,
                                          vm_abi::kCapabilityLiveFunctionIndexLayout) ||
        index.function_type_abi_key !=
            vm_abi::BuildCapabilityAbiKey(function_type_profile,
                                          vm_abi::kCapabilityFunctionTypeLayout)) {
        return false;
    }

    std::unordered_map<RuntimeImageId, uint32_t> live_counts;
    live_counts.reserve(current_images.size());
    for (const auto& function : index.live_function_infos) {
        if (function.function != 0 || function.code != 0 || function.code_object_pool != 0 ||
            function.owner_class != 0 || function.library != 0 || function.entry_kind_mask == 0 ||
            function.code_size == 0 || function.runtime_image_id == kInvalidRuntimeImageId) {
            return false;
        }
        const RuntimeImage* image = current_images.FindById(function.runtime_image_id);
        if (image == nullptr || !image->IsActive() ||
            function.loading_unit_id != image->loading_unit_id ||
            function.runtime_image_incarnation_epoch != image->incarnation_epoch ||
            function.engine_incarnation_epoch != image->engine_incarnation_epoch ||
            function.isolate_group_incarnation_epoch != image->isolate_group_incarnation_epoch ||
            function.runtime_generation != image->runtime_generation) {
            return false;
        }
        const auto record_entry = [&](uint64_t entry, uint8_t bit) {
            return (function.entry_kind_mask & bit) == 0 ||
                   image->ContainsRuntimeRange(static_cast<uintptr_t>(entry), 4);
        };
        if (!record_entry(function.code_entry_point, 1u << DARTPLANT_ENTRY_DEFAULT) ||
            !record_entry(function.code_unchecked_entry_point, 1u << DARTPLANT_ENTRY_UNCHECKED) ||
            !record_entry(function.code_monomorphic_entry_point,
                          1u << DARTPLANT_ENTRY_MONOMORPHIC) ||
            !record_entry(function.code_monomorphic_unchecked_entry_point,
                          1u << DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED)) {
            return false;
        }
        ++live_counts[function.runtime_image_id];
    }
    for (const auto& function : index.functions) {
        if (function.function_object != 0 || function.code_object != 0 ||
            function.code_object_pool != 0 || function.owner_class != 0 || function.library != 0 ||
            !function.live || function.runtime_entry == 0 || function.code_payload_start == 0 ||
            function.code_instructions_length == 0 ||
            function.runtime_image_id == kInvalidRuntimeImageId) {
            return false;
        }
        const RuntimeImage* image = current_images.FindById(function.runtime_image_id);
        if (image == nullptr || !image->IsActive() ||
            function.loading_unit_id != image->loading_unit_id ||
            function.runtime_image_incarnation_epoch != image->incarnation_epoch ||
            function.engine_incarnation_epoch != image->engine_incarnation_epoch ||
            function.isolate_group_incarnation_epoch != image->isolate_group_incarnation_epoch ||
            function.runtime_generation != image->runtime_generation ||
            !image->ContainsRuntimeRange(function.runtime_entry, 4) ||
            !image->ContainsRuntimeRange(function.code_payload_start,
                                         function.code_instructions_length)) {
            return false;
        }
    }
    const std::string function_type_abi_key =
        vm_abi::BuildCapabilityAbiKey(function_type_profile, vm_abi::kCapabilityFunctionTypeLayout);
    for (const auto& semantic : index.live_function_semantics) {
        const RuntimeImage* image = current_images.FindById(semantic.runtime_image_id);
        if (image == nullptr || !image->IsActive() ||
            semantic.runtime_image_incarnation_epoch != image->incarnation_epoch ||
            semantic.engine_incarnation_epoch != image->engine_incarnation_epoch ||
            semantic.isolate_group_incarnation_epoch != image->isolate_group_incarnation_epoch ||
            semantic.runtime_generation != image->runtime_generation ||
            semantic.function_type_profile_version !=
                function_type_profile.live_vm.profile_version ||
            semantic.function_type_abi_key != function_type_abi_key) {
            return false;
        }
    }
    for (const auto& image : current_images.images()) {
        if (!image.IsActive()) return false;
        const auto found = live_counts.find(image.id);
        const uint32_t observed = found == live_counts.end() ? 0 : found->second;
        if (observed != image.live_entry_count) return false;
        if (image.kind == RuntimeImageKind::kDeferred) {
            const bool fully_bound =
                image.deferred_program_hash_vm_bound &&
                image.deferred_load_state != RuntimeDeferredLoadState::kUnbound;
            if (fully_bound) continue;
            const bool pristine_addition =
                allow_unbound_deferred_additions && observed == 0 && image.live_entry_count == 0 &&
                !image.deferred_program_hash_vm_bound &&
                image.deferred_load_state == RuntimeDeferredLoadState::kUnbound;
            if (!pristine_addition) return false;
        }
    }
    return true;
}

bool CanReuseLiveSnapshotIndex(const SnapshotIndex& index, const RuntimeImageSet& current_images,
                               const RuntimeProfileRecord& live_index_profile,
                               const RuntimeProfileRecord& function_type_profile,
                               const DartPlantLiveVmFunctionIndexInfo& index_info) {
    return ValidateReusableLiveSnapshotIndex(index, current_images, live_index_profile,
                                             function_type_profile, index_info, false);
}

bool CanExtendLiveSnapshotIndex(const SnapshotIndex& index, const RuntimeImageSet& current_images,
                                const RuntimeProfileRecord& live_index_profile,
                                const RuntimeProfileRecord& function_type_profile,
                                const DartPlantLiveVmFunctionIndexInfo& index_info) {
    return ValidateReusableLiveSnapshotIndex(index, current_images, live_index_profile,
                                             function_type_profile, index_info, true);
}

const SnapshotFunction* SnapshotIndex::FindSnapshotFunction(
    std::string_view library_uri, std::string_view class_name, std::string_view function_name,
    std::string_view signature, DartPlantEntryKind entry_kind, bool* out_ambiguous) const {
    if (out_ambiguous != nullptr) *out_ambiguous = false;
    const SnapshotFunction* match = nullptr;
    for (const SnapshotFunction& function : functions) {
        if (function.library_uri != library_uri || function.class_name != class_name ||
            function.function_name != function_name || function.signature != signature ||
            function.entry_kind != entry_kind) {
            continue;
        }
        if (match != nullptr) {
            if (out_ambiguous != nullptr) *out_ambiguous = true;
            return nullptr;
        }
        match = &function;
    }
    return match;
}

const LiveFunctionSemanticSnapshot* SnapshotIndex::FindLiveFunctionSemanticSnapshot(
    uint64_t runtime_image_id, std::string_view library_uri, std::string_view class_name,
    std::string_view function_name) const {
    const LiveFunctionSemanticSnapshot* match = nullptr;
    for (const auto& candidate : live_function_semantics) {
        if ((runtime_image_id != 0 && candidate.runtime_image_id != runtime_image_id) ||
            candidate.library_uri != library_uri || candidate.class_name != class_name ||
            candidate.function_name != function_name) {
            continue;
        }
        if (match != nullptr) return nullptr;
        match = &candidate;
    }
    return match;
}

const RuntimeProfileRecord* ResolveLiveFunctionSemanticProfile(
    const LiveFunctionSemanticSnapshot& semantic) {
    const RuntimeProfileRecord* profile =
        FindRuntimeProfileByVersion(semantic.function_type_profile_version);
    if (profile == nullptr || semantic.function_type_abi_key.empty() ||
        semantic.function_type_abi_key !=
            vm_abi::BuildCapabilityAbiKey(*profile, vm_abi::kCapabilityFunctionTypeLayout)) {
        return nullptr;
    }
    return profile;
}

bool LiveFunctionSemanticMatchesOwner(const LiveFunctionSemanticSnapshot& semantic,
                                      const DartRuntimeOwnerIdentity& owner) {
    return semantic.runtime_generation == owner.runtime_generation &&
           semantic.engine_incarnation_epoch == owner.engine_incarnation_epoch &&
           semantic.isolate_group_incarnation_epoch == owner.isolate_group_incarnation_epoch &&
           semantic.runtime_image_id == owner.image_id &&
           semantic.runtime_image_incarnation_epoch == owner.image_incarnation_epoch;
}

SnapshotIndex BuildOfflineSnapshotIndexFromMetadata(const MetadataIndex& metadata) {
    SnapshotIndex index;
    index.module_name = metadata.module_name;
    index.snapshot_hash = metadata.snapshot_hash;
    index.build_id = metadata.build_id;
    index.dart_version = "metadata-cache";
    index.profile_version = "metadata-cache";
    for (const MethodRecord& method : metadata.methods) {
        if (method.address_kind != DARTPLANT_ADDRESS_SNAPSHOT_OFFSET) continue;
        index.functions.push_back({
            .library_uri = method.library_uri,
            .class_name = method.class_name,
            .function_name = method.function_name,
            .signature = method.signature,
            .entry_kind = method.entry_kind,
            .entry_va = method.section_va + method.address,
            .code_size = method.code_size,
            .code_section_va = method.section_va,
            .fingerprint = method.fingerprint,
        });
    }
    return index;
}

std::optional<SnapshotIndex> BuildSnapshotIndex(const DartPlantSnapshotIndexInfo& source,
                                                std::string* error) {
    if (source.struct_size < sizeof(source) || source.module_name == nullptr ||
        source.module_name[0] == '\0' || source.snapshot_hash == nullptr ||
        source.snapshot_hash[0] == '\0' || source.profile_version == nullptr ||
        source.profile_version[0] == '\0' || source.functions == nullptr ||
        source.function_count == 0) {
        if (error != nullptr) *error = "snapshot index header is invalid";
        return std::nullopt;
    }
    SnapshotIndex index;
    index.module_name = source.module_name;
    index.build_id = source.module_build_id == nullptr ? "" : source.module_build_id;
    index.snapshot_hash = source.snapshot_hash;
    index.dart_version = source.dart_version == nullptr ? "" : source.dart_version;
    index.profile_version = source.profile_version;
    index.functions.reserve(source.function_count);
    for (uint32_t position = 0; position < source.function_count; ++position) {
        const DartPlantSnapshotFunctionInfo& function = source.functions[position];
        constexpr size_t kSnapshotFunctionV1Size =
            offsetof(DartPlantSnapshotFunctionInfo, code_identity_proof);
        constexpr size_t kSnapshotFunctionV2Size =
            offsetof(DartPlantSnapshotFunctionInfo, function_kind);
        constexpr size_t kSnapshotFunctionV3Size =
            offsetof(DartPlantSnapshotFunctionInfo, code_payload_va);
        if (function.struct_size < kSnapshotFunctionV1Size || function.library_uri == nullptr ||
            function.function_name == nullptr || function.entry_va == 0 ||
            function.code_size == 0 || function.code_section_va > function.entry_va) {
            if (error != nullptr) *error = "snapshot function record is invalid";
            return std::nullopt;
        }
        DartPlantCodeIdentityProof identity_proof = DARTPLANT_CODE_IDENTITY_UNKNOWN;
        uint32_t physical_entry_alias_count = 0;
        if (function.struct_size >= kSnapshotFunctionV2Size) {
            identity_proof = function.code_identity_proof;
            physical_entry_alias_count = function.physical_entry_alias_count;
        }
        uint32_t function_kind = 0;
        bool closure_call_entry_only = false;
        if (function.struct_size >= kSnapshotFunctionV3Size) {
            function_kind = function.function_kind;
            closure_call_entry_only = function.closure_call_entry_only != 0;
        }
        uint64_t code_payload_va = 0;
        uint32_t code_instructions_length = 0;
        if (function.struct_size >= sizeof(function)) {
            if (function.code_instructions_length > UINT32_MAX) {
                if (error != nullptr) *error = "snapshot Code payload is too large";
                return std::nullopt;
            }
            code_payload_va = function.code_payload_va;
            code_instructions_length = static_cast<uint32_t>(function.code_instructions_length);
            if ((code_payload_va == 0) != (code_instructions_length == 0) ||
                (code_payload_va != 0 &&
                 (code_payload_va > function.entry_va ||
                  function.entry_va - code_payload_va >= code_instructions_length ||
                  function.code_size !=
                      code_instructions_length - (function.entry_va - code_payload_va)))) {
                if (error != nullptr) *error = "snapshot Code payload identity is inconsistent";
                return std::nullopt;
            }
        }
        if ((identity_proof == DARTPLANT_CODE_IDENTITY_UNIQUE && physical_entry_alias_count != 1) ||
            (identity_proof == DARTPLANT_CODE_IDENTITY_SHARED && physical_entry_alias_count < 2) ||
            (identity_proof != DARTPLANT_CODE_IDENTITY_UNKNOWN &&
             identity_proof != DARTPLANT_CODE_IDENTITY_UNIQUE &&
             identity_proof != DARTPLANT_CODE_IDENTITY_SHARED)) {
            if (error != nullptr) *error = "snapshot function identity proof is invalid";
            return std::nullopt;
        }
        index.functions.push_back({
            .library_uri = function.library_uri,
            .class_name = function.class_name == nullptr ? "" : function.class_name,
            .function_name = function.function_name,
            .signature = function.signature == nullptr ? "" : function.signature,
            .entry_kind = function.entry_kind,
            .entry_va = function.entry_va,
            .code_size = function.code_size,
            .code_section_va = function.code_section_va,
            .fingerprint = function.fingerprint == nullptr ? "" : function.fingerprint,
            .code_payload_va = code_payload_va,
            .code_instructions_length = code_instructions_length,
            .physical_entry_alias_count = physical_entry_alias_count,
            .code_identity_proof = identity_proof,
            .function_kind = function_kind,
            .closure_call_entry_only = closure_call_entry_only,
        });
    }
    return index;
}

std::optional<SnapshotIndex> BuildLiveSnapshotIndex(
    const DartPlantLiveVmContext& context, const DartPlantFlutterSnapshotInfo& snapshot,
    const RuntimeProfileRecord& live_index_profile,
    const RuntimeProfileRecord& function_type_profile, DartPlantVmAdapter* observation_adapter,
    const void* observation_lease, DartPlantLiveVmFunctionIndexInfo* out_info, std::string* error) {
    LiveVmInstructionImage image{};
    image.runtime_image_id = 0;
    image.loading_unit_id = 1;
    image.snapshot = snapshot;
    const std::array<LiveVmInstructionImage, 1> images = {image};
    return BuildLiveSnapshotIndexForImages(context, images, live_index_profile,
                                           function_type_profile, nullptr, observation_adapter,
                                           observation_lease, out_info, error);
}

std::optional<SnapshotIndex> BuildLiveSnapshotIndexForImages(
    const DartPlantLiveVmContext& context, std::span<const LiveVmInstructionImage> images,
    const RuntimeProfileRecord& profile, const RuntimeProfileRecord& function_type_profile,
    const RuntimeProfileRecord* deferred_profile, DartPlantVmAdapter* observation_adapter,
    const void* observation_lease, DartPlantLiveVmFunctionIndexInfo* out_info, std::string* error) {
    if (images.empty()) {
        if (error != nullptr) *error = "live snapshot index has no runtime images";
        return std::nullopt;
    }
    const LiveVmInstructionImage* root_image = nullptr;
    for (const auto& image : images) {
        if (image.loading_unit_id != 1) continue;
        if (root_image != nullptr) {
            if (error != nullptr) *error = "live snapshot index has multiple root images";
            return std::nullopt;
        }
        root_image = &image;
    }
    if (root_image == nullptr) {
        if (error != nullptr) *error = "live snapshot index has no root image";
        return std::nullopt;
    }
    const DartPlantFlutterSnapshotInfo& snapshot = root_image->snapshot;
    SnapshotIndex index;
    index.module_name = snapshot.module_name == nullptr ? "" : snapshot.module_name;
    index.module_path = snapshot.module_path == nullptr ? "" : snapshot.module_path;
    index.build_id = snapshot.module_build_id == nullptr ? "" : snapshot.module_build_id;
    index.snapshot_hash = snapshot.snapshot_hash == nullptr ? "" : snapshot.snapshot_hash;
    index.dart_version =
        profile.live_vm.dart_version == nullptr ? "" : profile.live_vm.dart_version;
    index.profile_version = profile.live_vm.name == nullptr ? "" : profile.live_vm.name;
    index.vm_profile_version = profile.live_vm.profile_version;
    index.live_index_abi_key =
        vm_abi::BuildCapabilityAbiKey(profile, vm_abi::kCapabilityLiveFunctionIndexLayout);
    index.function_type_abi_key =
        vm_abi::BuildCapabilityAbiKey(function_type_profile, vm_abi::kCapabilityFunctionTypeLayout);

    DartPlantLiveVmFunctionIndexInfo local_info{};
    local_info.struct_size = sizeof(local_info);
    std::vector<LiveVmFunctionSnapshotRecord> records;
    const DartPlantStatus status = CollectLiveVmFunctionSnapshotRecordsForImages(
        context, images, profile, function_type_profile, deferred_profile, observation_adapter,
        observation_lease, &records, &local_info);
    bool append_failed = false;
    if (status == DARTPLANT_OK) {
        for (const auto& record : records) {
            if (!AppendLiveSnapshotRecord(record, profile, function_type_profile, &index)) {
                append_failed = true;
                break;
            }
        }
    }
    if (status != DARTPLANT_OK || append_failed || index.functions.empty()) {
        if (error != nullptr) {
            *error = status != DARTPLANT_OK ? dartplant_last_error()
                     : append_failed ? "live VM Function index visitor rejected an entry family"
                                     : "live VM Function index is empty";
        }
        return std::nullopt;
    }
    if (out_info != nullptr) {
        if (out_info->struct_size < sizeof(DartPlantLiveVmFunctionIndexInfo)) {
            if (error != nullptr) *error = "live VM Function index info is too small";
            return std::nullopt;
        }
        *out_info = local_info;
    }
    return index;
}

std::optional<SnapshotIndex> BuildDeferredLiveSnapshotIndexIncrement(
    const SnapshotIndex& base, const DartPlantLiveVmContext& context,
    std::span<const LiveVmInstructionImage> images,
    std::span<const uint64_t> changed_runtime_image_ids,
    const RuntimeProfileRecord& live_index_profile,
    const RuntimeProfileRecord& function_type_profile, const RuntimeProfileRecord& deferred_profile,
    DartPlantVmAdapter* observation_adapter, const void* observation_lease,
    const DartPlantLiveVmFunctionIndexInfo& base_info, DartPlantLiveVmFunctionIndexInfo* out_info,
    std::string* error) {
    if (out_info == nullptr || changed_runtime_image_ids.empty() || !base.stable_live_directory ||
        base_info.struct_size < sizeof(DartPlantLiveVmFunctionIndexInfo) ||
        base_info.function_count != base.live_function_infos.size()) {
        if (error != nullptr) *error = "stable live Function delta base is invalid";
        return std::nullopt;
    }
    const std::string expected_live_abi = vm_abi::BuildCapabilityAbiKey(
        live_index_profile, vm_abi::kCapabilityLiveFunctionIndexLayout);
    const std::string expected_function_type_abi =
        vm_abi::BuildCapabilityAbiKey(function_type_profile, vm_abi::kCapabilityFunctionTypeLayout);
    if (base.vm_profile_version != live_index_profile.live_vm.profile_version ||
        base.live_index_abi_key != expected_live_abi ||
        base.function_type_abi_key != expected_function_type_abi) {
        if (error != nullptr) *error = "stable live Function delta capability ABI changed";
        return std::nullopt;
    }

    std::unordered_set<uint64_t> target_images;
    target_images.reserve(changed_runtime_image_ids.size());
    for (const uint64_t image_id : changed_runtime_image_ids) {
        if (image_id == kInvalidRuntimeImageId || !target_images.insert(image_id).second) {
            if (error != nullptr) *error = "stable live Function delta image set is invalid";
            return std::nullopt;
        }
    }

    // Slotless records are safe to carry forward only when their owning image
    // is unchanged. If a target image already contributed one, there is no
    // durable owner slot with which the delta can prove that the old record
    // was replaced rather than duplicated or left stale. Normal deferred
    // NotLoaded -> Loaded starts from live_entry_count == 0, so rejecting this
    // state does not penalize the supported additive fast path.
    for (const auto& function : base.live_function_infos) {
        if (target_images.contains(function.runtime_image_id) && !HasStableOwnerSlot(function)) {
            if (error != nullptr) {
                *error = "changed deferred image contains a slotless stable-directory record";
            }
            return std::nullopt;
        }
    }

    std::vector<LiveVmFunctionSnapshotRecord> records;
    DartPlantLiveVmFunctionIndexInfo delta_info{};
    delta_info.struct_size = sizeof(delta_info);
    const DartPlantStatus status = CollectLiveVmDeferredFunctionSnapshotRecordsForImages(
        context, images, changed_runtime_image_ids, live_index_profile, function_type_profile,
        deferred_profile, observation_adapter, observation_lease, &records, &delta_info);
    if (status != DARTPLANT_OK) {
        if (error != nullptr) {
            const char* detail = dartplant_last_error();
            *error = detail == nullptr || detail[0] == '\0'
                         ? "failed to capture deferred live Function delta"
                         : detail;
        }
        return std::nullopt;
    }
    if (delta_info.skipped_function_count != 0) {
        if (error != nullptr) *error = "deferred live Function delta skipped a changed Function";
        return std::nullopt;
    }

    std::unordered_set<uint64_t> base_slots;
    base_slots.reserve(base.live_function_infos.size());
    for (const auto& function : base.live_function_infos) {
        if (!HasStableOwnerSlot(function)) continue;
        if (!base_slots.insert(StableOwnerSlotKey(function)).second) {
            if (error != nullptr)
                *error = "stable live Function directory has duplicate owner slots";
            return std::nullopt;
        }
    }

    std::unordered_set<uint64_t> changed_slots;
    changed_slots.reserve(records.size());
    for (const auto& record : records) {
        if (!HasStableOwnerSlot(record.function) ||
            !target_images.contains(record.function.runtime_image_id) ||
            !changed_slots.insert(StableOwnerSlotKey(record.function)).second) {
            if (error != nullptr)
                *error = "deferred live Function delta has ambiguous owner-slot identity";
            return std::nullopt;
        }
    }

    SnapshotIndex index = base;
    index.stable_live_directory = false;
    const auto slot_changed = [&changed_slots](uint32_t class_id, uint32_t function_index) {
        return class_id != 0 && function_index != UINT32_MAX &&
               changed_slots.contains(StableOwnerSlotKey(class_id, function_index));
    };
    std::erase_if(index.live_function_infos, [&](const DartPlantLiveVmFunctionInfo& function) {
        return slot_changed(function.owner_class_id, function.owner_function_index);
    });
    std::erase_if(index.functions, [&](const SnapshotFunction& function) {
        return slot_changed(function.owner_class_id, function.owner_function_index);
    });
    std::erase_if(index.live_function_semantics, [&](const LiveFunctionSemanticSnapshot& semantic) {
        return slot_changed(semantic.owner_class_id, semantic.owner_function_index);
    });

    for (const auto& record : records) {
        if (!AppendLiveSnapshotRecord(record, live_index_profile, function_type_profile, &index)) {
            if (error != nullptr) *error = "deferred live Function delta rejected an entry family";
            return std::nullopt;
        }
    }
    if (index.live_function_infos.empty() || index.functions.empty() ||
        !RecomputeLiveDirectoryAliasMetadata(&index, base_info.skipped_function_count, out_info)) {
        if (error != nullptr)
            *error = "deferred live Function delta produced an inconsistent stable directory";
        return std::nullopt;
    }
    StabilizeLiveSnapshotIndexForPublication(&index);
    return index;
}

}  // namespace dartplant
