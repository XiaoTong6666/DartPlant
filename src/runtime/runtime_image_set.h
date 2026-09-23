// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_RUNTIME_RUNTIME_IMAGE_SET_H_
#define DARTPLANT_RUNTIME_RUNTIME_IMAGE_SET_H_

#include <stdint.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/internal.h"
#include "runtime/flutter_snapshot_internal.h"

namespace dartplant {

using RuntimeImageId = uint64_t;
constexpr RuntimeImageId kInvalidRuntimeImageId = 0;

using RuntimeImageIncarnationEpoch = uint64_t;
constexpr RuntimeImageIncarnationEpoch kInvalidRuntimeImageIncarnationEpoch = 0;

enum class RuntimeImageKind : uint8_t {
    kRoot = 0,
    kDeferred,
};

enum class RuntimeImageLifecycleState : uint8_t {
    kStaged = 0,
    kActive,
    kDraining,
    kRetired,
};

enum class RuntimeDeferredLoadState : uint8_t {
    kUnbound = 0,
    kNotLoaded,
    kLoaded,
};

// One independently mapped Dart AOT instruction namespace. A runtime image is
// deliberately stronger than ModuleImage: it binds the ELF artifact identity
// to the exact Dart isolate-instructions symbol that defines the entry-VA
// coordinate system for Functions in this image.
struct RuntimeImage {
    RuntimeImageId id = kInvalidRuntimeImageId;
    RuntimeImageIncarnationEpoch incarnation_epoch = kInvalidRuntimeImageIncarnationEpoch;
    RuntimeImageKind kind = RuntimeImageKind::kRoot;
    // Dart LoadingUnit::kRootId is 1. A zero id means the host/packager did not
    // expose a source-proven loading-unit id and the image may only be selected
    // by its exact module/instruction identity.
    uint32_t loading_unit_id = 0;
    uint64_t runtime_generation = 0;
    uint64_t engine_incarnation_epoch = 0;
    uint64_t isolate_group_incarnation_epoch = 0;
    RuntimeImageLifecycleState lifecycle = RuntimeImageLifecycleState::kStaged;
    // Number of retained live Dart Function entry families observed from the
    // currently bound isolate-group object graph whose Code payload resolves
    // uniquely into this image. A non-zero count is the semantic binding that
    // upgrades a merely source-proven physical image into an image known to be
    // owned by the current isolate group.
    uint32_t live_entry_count = 0;
    // Deferred-only semantic proof. True only after the source-verified
    // serialized unit hash has been compared with the current isolate group's
    // ObjectStore.loading_units[0] root program hash.
    bool deferred_program_hash_vm_bound = false;
    // Deferred loading completion mutates Function entry-point caches without
    // changing the mapped RuntimeImage set. Cache reuse therefore needs one
    // cheap VM-semantic receipt in addition to image identity. This state is
    // derived from LoadingUnit.base_objects under the exact moving-GC
    // observation lease: null means the unit snapshot has not completed,
    // non-null means UnitDeserializationRoots::PostLoad has published the
    // deserialized reference set after entry points were updated.
    RuntimeDeferredLoadState deferred_load_state = RuntimeDeferredLoadState::kUnbound;
    ModuleImage module;
    FlutterSnapshotSource snapshot;

    bool ContainsRuntimeRange(uintptr_t address, size_t size = 1) const;
    std::optional<uint64_t> RuntimeAddressToElfVa(uintptr_t address, size_t size = 1) const;
    bool IsActive() const { return lifecycle == RuntimeImageLifecycleState::kActive; }
    DartRuntimeOwnerIdentity OwnerIdentity() const {
        return {
            .runtime_generation = runtime_generation,
            .engine_incarnation_epoch = engine_incarnation_epoch,
            .isolate_group_incarnation_epoch = isolate_group_incarnation_epoch,
            .image_id = id,
            .image_incarnation_epoch = incarnation_epoch,
        };
    }
};

class RuntimeImageSet final {
public:
    void Clear();

    // Installs the root loading unit. Replacing it clears every deferred image
    // because loading-unit identity is meaningful only inside one root program
    // incarnation.
    bool SetRoot(const ModuleImage& module, const FlutterSnapshotSource& snapshot,
                 uint64_t runtime_generation, std::string* error = nullptr);

    // Adds one source-verified secondary loading unit. Duplicate loading-unit
    // ids, duplicate module incarnations with different instruction identity,
    // and overlapping instruction ranges fail closed.
    bool AddDeferred(const ModuleImage& module, const FlutterSnapshotSource& snapshot,
                     uint32_t loading_unit_id, uint64_t runtime_generation,
                     std::string* error = nullptr);

    const RuntimeImage* Root() const;
    const RuntimeImage* FindById(RuntimeImageId id) const;
    const RuntimeImage* FindByLoadingUnitId(uint32_t loading_unit_id) const;
    const RuntimeImage* FindByRuntimeRange(uintptr_t address, size_t size = 1) const;
    const RuntimeImage* FindByModule(const ModuleImage& module) const;

    std::span<const RuntimeImage> images() const { return images_; }
    size_t size() const { return images_.size(); }
    bool empty() const { return images_.empty(); }

    // Compares only source/provenance identity. RuntimeImageId itself is a
    // per-set handle and is intentionally not part of incarnation equality.
    bool SameIdentity(const RuntimeImageSet& other) const;
    // Keeps ids stable for images that survive an in-generation image-set
    // refresh and assigns fresh ids to newly loaded secondary images.
    bool PreserveIdsFrom(const RuntimeImageSet& previous);
    // Reconciles logical image ids and physical incarnation epochs against the
    // previous published set. Exact physical survivors preserve both id and
    // epoch. A replacement for the same logical loading unit preserves only
    // its id and receives a fresh epoch. New logical units receive both a new
    // id and a new epoch. Allocators are monotonic across refreshes so a stale
    // owner token can never become current again through numeric reuse.
    bool ReconcileOwnershipFrom(const RuntimeImageSet& previous);
    // Carries VM-semantic receipts only for physical RuntimeImage owners that
    // survive an in-generation image-set transition unchanged. Newly-added or
    // replaced images remain unbound and must be proven by the next live VM
    // observation before they can inherit any Function-directory semantics.
    void PreserveSemanticBindingsFrom(const RuntimeImageSet& previous);
    bool ContainsIdentity(const RuntimeImage& image) const;
    bool RemoveById(RuntimeImageId id);
    void BindGeneration(uint64_t runtime_generation);
    void BindOwnerEpochs(uint64_t engine_incarnation_epoch,
                         uint64_t isolate_group_incarnation_epoch);
    void ActivateAll();
    bool BeginDrain(RuntimeImageId id, RuntimeImageIncarnationEpoch incarnation_epoch);
    bool Retire(RuntimeImageId id, RuntimeImageIncarnationEpoch incarnation_epoch);
    void ResetLiveEntryBindings();
    void ResetSemanticBindings();
    bool RecordLiveEntry(RuntimeImageId id);
    // Transactionally replaces semantic live-entry bindings. Every id must
    // refer to an image in this set; otherwise the existing counts are left
    // untouched. Duplicate ids deliberately count distinct retained Function
    // entry families rather than distinct physical Code payloads.
    bool BindLiveEntries(std::span<const RuntimeImageId> image_ids);
    // Binds every currently loaded deferred image to the VM's authoritative
    // root program hash. The operation is transactional and never publishes a
    // partial set of proven units.
    bool BindDeferredProgramHash(uint32_t root_program_hash);
    // Binds one observation-scoped deferred load-state receipt to the exact
    // RuntimeImage/loading-unit owner. Callers normally apply these to a copy
    // of the image set and publish that copy only after every unit succeeds.
    bool BindDeferredLoadState(RuntimeImageId id, uint32_t loading_unit_id, bool loaded);
    // Compares only the deferred VM load-state receipts for otherwise exact
    // image owners. A change means the stable Function directory may be stale
    // even though the mapped image set itself did not change.
    bool SameDeferredLoadStates(const RuntimeImageSet& other) const;

private:
    RuntimeImageId AllocateId();
    RuntimeImageIncarnationEpoch AllocateIncarnationEpoch();

    RuntimeImageId next_id_ = 1;
    RuntimeImageIncarnationEpoch next_incarnation_epoch_ = 1;
    RuntimeImageId root_id_ = kInvalidRuntimeImageId;
    std::vector<RuntimeImage> images_;
};

// Dart gen_snapshot emits secondary ELF loading units as
//   <main-filename>-<id>.part.so
// (root unit id 1, deferred ids >1). Flutter packagers may drop the inner
// ".so" before the suffix, so accept both libapp.so-2.part.so and
// libapp-2.part.so. This is only a producer-shape candidate filter; the module
// still has to pass full FlutterSnapshotSource provenance validation.
std::optional<uint32_t> ParseDeferredLoadingUnitId(std::string_view root_module_name,
                                                   std::string_view candidate_module_name);

}  // namespace dartplant

#endif  // DARTPLANT_RUNTIME_RUNTIME_IMAGE_SET_H_
