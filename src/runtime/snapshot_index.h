#ifndef DARTPLANT_RUNTIME_SNAPSHOT_INDEX_H_
#define DARTPLANT_RUNTIME_SNAPSHOT_INDEX_H_

#include <stdint.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dartplant/advanced/flutter_snapshot.h"
#include "dartplant/advanced/live_vm.h"
#include "dartplant/vm_adapter.h"
#include "runtime/runtime_image_set.h"
#include "vm/runtime_profiles.h"

namespace dartplant {

struct MetadataIndex;
struct LiveVmInstructionImage;
struct DartRuntimeOwnerIdentity;

struct LiveFunctionSemanticSnapshot {
    uint64_t runtime_image_id = 0;
    uint64_t runtime_image_incarnation_epoch = 0;
    uint64_t engine_incarnation_epoch = 0;
    uint64_t isolate_group_incarnation_epoch = 0;
    uint64_t runtime_generation = 0;
    uint32_t owner_class_id = 0;
    uint32_t owner_function_index = UINT32_MAX;
    std::string library_uri;
    std::string class_name;
    std::string function_name;
    uint32_t function_type_profile_version = 0;
    std::string function_type_abi_key;
    DartPlantDartFunctionSignatureInfo signature{};
    std::vector<DartPlantDartParameterInfo> parameters;
};

struct SnapshotFunction {
    uint64_t runtime_image_id = 0;
    uint64_t runtime_image_incarnation_epoch = 0;
    uint64_t engine_incarnation_epoch = 0;
    uint64_t isolate_group_incarnation_epoch = 0;
    uint64_t runtime_generation = 0;
    uint32_t loading_unit_id = 0;
    uint32_t owner_class_id = 0;
    uint32_t owner_function_index = UINT32_MAX;
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
    uint32_t vm_profile_version = 0;
    // Capability identities used to build the live directory. These are
    // stable ABI descriptors rather than whole-profile authority and allow a
    // later observation of the same owner to reuse the immutable directory
    // without rescanning the moving heap.
    std::string live_index_abi_key;
    std::string function_type_abi_key;
    // Published live directories are reusable only after every movable Dart
    // heap receipt has been stripped. Keep this as an explicit state bit so a
    // synthetic/legacy index with coincidentally-null object fields can never
    // enter the reuse fast path.
    bool stable_live_directory = false;
    std::vector<SnapshotFunction> functions;
    // Live VM enumeration already produces one complete entry-family record
    // per Dart Function. Preserve that canonical record so indexed public
    // queries do not need to reconstruct it by rescanning the flattened
    // SnapshotFunction list for every position.
    std::vector<DartPlantLiveVmFunctionInfo> live_function_infos;
    // FunctionType is a GC-managed heap object. Capture its semantic value
    // while the live index is built inside a moving-GC observation lease;
    // later runtime APIs must consume this immutable copy rather than
    // dereference a cached movable FunctionPtr.
    std::vector<LiveFunctionSemanticSnapshot> live_function_semantics;

    const SnapshotFunction* FindSnapshotFunction(std::string_view library_uri,
                                                 std::string_view class_name,
                                                 std::string_view function_name,
                                                 std::string_view signature,
                                                 DartPlantEntryKind entry_kind,
                                                 bool* out_ambiguous = nullptr) const;
    const LiveFunctionSemanticSnapshot* FindLiveFunctionSemanticSnapshot(
        uint64_t runtime_image_id, std::string_view library_uri, std::string_view class_name,
        std::string_view function_name) const;
};

const RuntimeProfileRecord* ResolveLiveFunctionSemanticProfile(
    const LiveFunctionSemanticSnapshot& semantic);
bool LiveFunctionSemanticMatchesOwner(const LiveFunctionSemanticSnapshot& semantic,
                                      const DartRuntimeOwnerIdentity& owner);

// Compatibility cache only. A runtime snapshot parser must populate the same
// model without calling this function. The runtime marks this source so callers
// can distinguish it from a real snapshot backend.
SnapshotIndex BuildOfflineSnapshotIndexFromMetadata(const MetadataIndex& metadata);
std::optional<SnapshotIndex> BuildSnapshotIndex(const DartPlantSnapshotIndexInfo& source,
                                                std::string* error);
std::optional<SnapshotIndex> BuildLiveSnapshotIndex(
    const DartPlantLiveVmContext& context, const DartPlantFlutterSnapshotInfo& snapshot,
    const RuntimeProfileRecord& live_index_profile,
    const RuntimeProfileRecord& function_type_profile, DartPlantVmAdapter* observation_adapter,
    const void* observation_lease, DartPlantLiveVmFunctionIndexInfo* out_info, std::string* error);
std::optional<SnapshotIndex> BuildLiveSnapshotIndexForImages(
    const DartPlantLiveVmContext& context, std::span<const LiveVmInstructionImage> images,
    const RuntimeProfileRecord& live_index_profile,
    const RuntimeProfileRecord& function_type_profile, const RuntimeProfileRecord* deferred_profile,
    DartPlantVmAdapter* observation_adapter, const void* observation_lease,
    DartPlantLiveVmFunctionIndexInfo* out_info, std::string* error);

// Incrementally refreshes only newly-loaded deferred RuntimeImages. The base
// directory must already be stable and owner/ABI-valid for the pre-load image
// set. Exact stable owner slots (Class.functions for ordinary Functions or
// loading-unit InstructionsTable.code_objects for deferred slotless Functions)
// replace affected records; if that proof is incomplete or ambiguous the
// caller must fall back to a full live-graph rebuild.
std::optional<SnapshotIndex> BuildDeferredLiveSnapshotIndexIncrement(
    const SnapshotIndex& base, const DartPlantLiveVmContext& context,
    std::span<const LiveVmInstructionImage> images,
    std::span<const uint64_t> changed_runtime_image_ids,
    const RuntimeProfileRecord& live_index_profile,
    const RuntimeProfileRecord& function_type_profile, const RuntimeProfileRecord& deferred_profile,
    DartPlantVmAdapter* observation_adapter, const void* observation_lease,
    const DartPlantLiveVmFunctionIndexInfo& base_info, DartPlantLiveVmFunctionIndexInfo* out_info,
    std::string* error);

// Internal record adapter shared by the live-VM visitor and host regression
// tests. Returns false rather than publishing a partial entry family.
bool AppendLiveSnapshotFunctionRecord(const DartPlantLiveVmFunctionInfo& function,
                                      uint32_t profile_version, SnapshotIndex* index);

// Validates that the canonical live Function entry-family records and the
// flattened per-entry SnapshotFunction records agree on one source-proven
// RuntimeImage/loading-unit identity. On success, publishes one semantic
// binding count per retained Dart Function into a copy of the current image
// set. The input image set is never partially modified on failure.
bool BindLiveSnapshotImageSemantics(const SnapshotIndex& index,
                                    const RuntimeImageSet& current_images,
                                    RuntimeImageSet* out_images, std::string* error);

// Rebinds per-image live Function counts from an already-stabilized directory.
// Unlike BindLiveSnapshotImageSemantics(), this never requires or consumes raw
// GC-managed Function/Code receipts.
bool BindStableLiveSnapshotImageSemantics(const SnapshotIndex& index,
                                          const RuntimeImageSet& current_images,
                                          RuntimeImageSet* out_images, std::string* error);

// Drops GC-managed object addresses after the observation-scoped index has
// been validated and bound to RuntimeImage ownership. Executable entry/payload
// addresses, source identity and immutable FunctionType semantics remain.
// Persistent runtime consumers must never rely on FunctionPtr/CodePtr/etc.
// surviving a compacting GC.
void StabilizeLiveSnapshotIndexForPublication(SnapshotIndex* index);

// Removes records owned by exact retired RuntimeImage incarnations from an
// already-stabilized live directory. This is used when a secondary deferred
// image is unloaded/remapped: surviving root/sibling semantics remain valid
// and need not be rebuilt from the moving heap. The operation is
// transactional; on failure neither the index nor its aggregate info changes.
bool PruneStableLiveSnapshotIndexForImages(SnapshotIndex* index,
                                           std::span<const RuntimeImage> retired_images,
                                           DartPlantLiveVmFunctionIndexInfo* index_info,
                                           std::string* error);

// True only when a previously published stable live directory belongs to the
// exact current RuntimeImage owners and was built from the same capability
// ABIs. This intentionally performs only native/cache validation; it never
// dereferences a Dart heap object.
bool CanReuseLiveSnapshotIndex(const SnapshotIndex& index, const RuntimeImageSet& current_images,
                               const RuntimeProfileRecord& live_index_profile,
                               const RuntimeProfileRecord& function_type_profile,
                               const DartPlantLiveVmFunctionIndexInfo& index_info);

// Weaker form used only as a base for an incremental deferred-image refresh.
// Existing indexed RuntimeImage owners must still match exactly, but newly
// added deferred images may remain entirely unbound (zero live entries,
// Unbound load state, no VM program-hash receipt). The caller must observe and
// bind those images before publishing the directory again.
bool CanExtendLiveSnapshotIndex(const SnapshotIndex& index, const RuntimeImageSet& current_images,
                                const RuntimeProfileRecord& live_index_profile,
                                const RuntimeProfileRecord& function_type_profile,
                                const DartPlantLiveVmFunctionIndexInfo& index_info);

}  // namespace dartplant

#endif  // DARTPLANT_RUNTIME_SNAPSHOT_INDEX_H_
