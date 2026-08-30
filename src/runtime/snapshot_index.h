#ifndef DARTPLANT_RUNTIME_SNAPSHOT_INDEX_H_
#define DARTPLANT_RUNTIME_SNAPSHOT_INDEX_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dartplant/advanced/flutter_snapshot.h"
#include "dartplant/advanced/live_vm.h"

namespace dartplant {

struct MetadataIndex;

struct SnapshotFunction {
    std::string library_uri;
    std::string class_name;
    std::string function_name;
    std::string signature;
    DartPlantEntryKind entry_kind = DARTPLANT_ENTRY_DEFAULT;
    uint64_t entry_va = 0;
    uint64_t code_size = 0;
    uint64_t code_section_va = 0;
    std::string fingerprint;
    uint64_t function_object = 0;
    uint64_t code_object = 0;
    uint64_t code_object_pool = 0;
    uint64_t code_payload_va = 0;
    uint64_t code_payload_start = 0;
    uint32_t code_instructions_length = 0;
    uintptr_t runtime_entry = 0;
    uintptr_t code_entry = 0;
    uint64_t owner_class = 0;
    uint64_t library = 0;
    uint32_t entry_alias_count = 1;
    uint32_t physical_entry_alias_count = 0;
    DartPlantCodeIdentityProof code_identity_proof = DARTPLANT_CODE_IDENTITY_UNKNOWN;
    uint32_t function_kind = 0;
    bool closure_call_entry_only = false;
    bool owner_is_toplevel_class = false;
    bool code_owner_matches_function = false;
    bool live = false;
};

struct SnapshotIndex {
    std::string module_name;
    std::string module_path;
    std::string snapshot_hash;
    std::string build_id;
    std::string dart_version;
    std::string profile_version;
    std::vector<SnapshotFunction> functions;

    const SnapshotFunction* FindSnapshotFunction(std::string_view library_uri,
                                                 std::string_view class_name,
                                                 std::string_view function_name,
                                                 std::string_view signature,
                                                 DartPlantEntryKind entry_kind,
                                                 bool* out_ambiguous = nullptr) const;
};

// Compatibility cache only. A runtime snapshot parser must populate the same
// model without calling this function. The runtime marks this source so callers
// can distinguish it from a real snapshot backend.
SnapshotIndex BuildOfflineSnapshotIndexFromMetadata(const MetadataIndex& metadata);
std::optional<SnapshotIndex> BuildSnapshotIndex(const DartPlantSnapshotIndexInfo& source,
                                                std::string* error);
std::optional<SnapshotIndex> BuildLiveSnapshotIndex(const DartPlantLiveVmContext& context,
                                                    const DartPlantFlutterSnapshotInfo& snapshot,
                                                    DartPlantLiveVmFunctionIndexInfo* out_info,
                                                    std::string* error);

// Internal record adapter shared by the live-VM visitor and host regression
// tests. Returns false rather than publishing a partial entry family.
bool AppendLiveSnapshotFunctionRecord(const DartPlantLiveVmFunctionInfo& function,
                                      uint32_t profile_version, SnapshotIndex* index);

}  // namespace dartplant

#endif  // DARTPLANT_RUNTIME_SNAPSHOT_INDEX_H_
