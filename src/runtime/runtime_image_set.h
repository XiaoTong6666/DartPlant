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

enum class RuntimeImageKind : uint8_t {
    kRoot = 0,
    kDeferred,
};

// One independently mapped Dart AOT instruction namespace. A runtime image is
// deliberately stronger than ModuleImage: it binds the ELF artifact identity
// to the exact Dart isolate-instructions symbol that defines the entry-VA
// coordinate system for Functions in this image.
struct RuntimeImage {
    RuntimeImageId id = kInvalidRuntimeImageId;
    RuntimeImageKind kind = RuntimeImageKind::kRoot;
    // Dart LoadingUnit::kRootId is 1. A zero id means the host/packager did not
    // expose a source-proven loading-unit id and the image may only be selected
    // by its exact module/instruction identity.
    uint32_t loading_unit_id = 0;
    uint64_t runtime_generation = 0;
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
    ModuleImage module;
    FlutterSnapshotSource snapshot;

    bool ContainsRuntimeRange(uintptr_t address, size_t size = 1) const;
    std::optional<uint64_t> RuntimeAddressToElfVa(uintptr_t address, size_t size = 1) const;
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
    void BindGeneration(uint64_t runtime_generation);
    void ResetLiveEntryBindings();
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

private:
    RuntimeImageId AllocateId();

    RuntimeImageId next_id_ = 1;
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
