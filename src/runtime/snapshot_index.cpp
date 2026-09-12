#include "runtime/snapshot_index.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <unordered_map>

#include "core/internal.h"
#include "vm/live_vm_internal.h"
#include "vm/runtime_profiles.h"

namespace dartplant {
namespace {

struct LiveSnapshotBuildState {
    SnapshotIndex* index = nullptr;
    const RuntimeProfileRecord* profile = nullptr;
    bool failed = false;
};

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

uint8_t AppendLiveSnapshotFunction(const DartPlantLiveVmFunctionInfo* function, void* user_data) {
    auto* state = static_cast<LiveSnapshotBuildState*>(user_data);
    if (function == nullptr || state == nullptr || state->index == nullptr) return 0;
    if (state->profile == nullptr ||
        !AppendLiveSnapshotFunctionRecord(*function, state->profile->live_vm.profile_version,
                                          state->index)) {
        state->failed = true;
        return 0;
    }
    return 1;
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
            .loading_unit_id = function.loading_unit_id,
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
        if (image == nullptr || function.loading_unit_id != image->loading_unit_id) {
            if (error != nullptr)
                *error = "live Function entry family disagrees with runtime image namespace";
            return false;
        }
        const auto [_, inserted] =
            families.emplace(function.function, FamilyState{
                                                    .image_id = function.runtime_image_id,
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

std::optional<SnapshotIndex> BuildLiveSnapshotIndex(const DartPlantLiveVmContext& context,
                                                    const DartPlantFlutterSnapshotInfo& snapshot,
                                                    const RuntimeProfileRecord& profile,
                                                    DartPlantLiveVmFunctionIndexInfo* out_info,
                                                    std::string* error) {
    LiveVmInstructionImage image{};
    image.runtime_image_id = 0;
    image.loading_unit_id = 1;
    image.snapshot = snapshot;
    const std::array<LiveVmInstructionImage, 1> images = {image};
    return BuildLiveSnapshotIndexForImages(context, images, profile, out_info, error);
}

std::optional<SnapshotIndex> BuildLiveSnapshotIndexForImages(
    const DartPlantLiveVmContext& context, std::span<const LiveVmInstructionImage> images,
    const RuntimeProfileRecord& profile, DartPlantLiveVmFunctionIndexInfo* out_info,
    std::string* error) {
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

    LiveSnapshotBuildState state{.index = &index, .profile = &profile};
    DartPlantLiveVmFunctionIndexInfo local_info{};
    local_info.struct_size = sizeof(local_info);
    const DartPlantStatus status = VisitLiveVmFunctionsForImages(
        context, images, profile, AppendLiveSnapshotFunction, &state, &local_info);
    if (status != DARTPLANT_OK || state.failed || index.functions.empty()) {
        if (error != nullptr) {
            *error = status != DARTPLANT_OK ? dartplant_last_error()
                     : state.failed ? "live VM Function index visitor rejected an entry family"
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

}  // namespace dartplant
