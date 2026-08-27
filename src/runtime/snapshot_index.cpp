#include "runtime/snapshot_index.h"

#include <algorithm>

#include "core/internal.h"

namespace dartplant {
namespace {

struct LiveSnapshotBuildState {
    SnapshotIndex* index = nullptr;
};

uint8_t AppendLiveSnapshotFunction(const DartPlantLiveVmFunctionInfo* function, void* user_data) {
    auto* state = static_cast<LiveSnapshotBuildState*>(user_data);
    if (function == nullptr || state == nullptr || state->index == nullptr) return 0;
    state->index->functions.push_back({
        .library_uri = function->library_uri,
        .class_name = function->class_name,
        .function_name = function->function_name,
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
        .entry_va = function->entry_va,
        .code_size = function->code_size,
        .code_section_va = function->code_section_va,
        .fingerprint = "",
        .function_object = function->function,
        .code_object = function->code,
        .code_object_pool = function->code_object_pool,
        .runtime_entry = static_cast<uintptr_t>(function->function_entry_point),
        .code_entry = static_cast<uintptr_t>(function->code_entry_point),
        .owner_class = function->owner_class,
        .library = function->library,
        .entry_alias_count = function->entry_alias_count,
        .function_kind = function->function_kind,
        .owner_is_toplevel_class = function->owner_is_toplevel_class != 0,
        .code_owner_matches_function = function->code_owner_matches_function != 0,
        .live = true,
    });
    return 1;
}

}  // namespace

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
        if (function.struct_size < sizeof(function) || function.library_uri == nullptr ||
            function.function_name == nullptr || function.entry_va == 0 ||
            function.code_size == 0 || function.code_section_va > function.entry_va) {
            if (error != nullptr) *error = "snapshot function record is invalid";
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
        });
    }
    return index;
}

std::optional<SnapshotIndex> BuildLiveSnapshotIndex(const DartPlantLiveVmContext& context,
                                                    const DartPlantFlutterSnapshotInfo& snapshot,
                                                    DartPlantLiveVmFunctionIndexInfo* out_info,
                                                    std::string* error) {
    SnapshotIndex index;
    index.module_name = snapshot.module_name == nullptr ? "" : snapshot.module_name;
    index.module_path = snapshot.module_path == nullptr ? "" : snapshot.module_path;
    index.build_id = snapshot.module_build_id == nullptr ? "" : snapshot.module_build_id;
    index.snapshot_hash = snapshot.snapshot_hash == nullptr ? "" : snapshot.snapshot_hash;
    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    const DartPlantStatus profile_status = dartplant_live_vm_select_profile(&snapshot, &profile);
    if (profile_status != DARTPLANT_OK) {
        if (error != nullptr) *error = dartplant_last_error();
        return std::nullopt;
    }
    index.dart_version = profile.dart_version == nullptr ? "" : profile.dart_version;
    index.profile_version = profile.name == nullptr ? "" : profile.name;

    LiveSnapshotBuildState state{.index = &index};
    DartPlantLiveVmFunctionIndexInfo local_info{};
    local_info.struct_size = sizeof(local_info);
    const DartPlantStatus status = dartplant_live_vm_visit_functions(
        &context, &snapshot, AppendLiveSnapshotFunction, &state, &local_info);
    if (status != DARTPLANT_OK || index.functions.empty()) {
        if (error != nullptr) {
            *error =
                status == DARTPLANT_OK ? "live VM Function index is empty" : dartplant_last_error();
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
