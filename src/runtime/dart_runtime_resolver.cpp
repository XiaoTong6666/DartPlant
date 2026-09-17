// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>
#include <unordered_map>

#include "abi/value_codec.h"
#include "runtime/default_runtime.h"
#include "runtime/runtime_image_transition.h"
#include "runtime/runtime_internal.h"
#include "vm/abi/resolver.h"
#include "vm/live_vm_internal.h"
#include "vm/runtime_profiles.h"

namespace dartplant {

struct RuntimeRegistration {
    DartPlantRuntime* runtime = nullptr;
    std::string app_module_name;
    std::string runtime_module_name;
    std::atomic_bool closing{false};
    size_t active_operations = 0;
    std::condition_variable idle;
};

bool SameRuntimeIsolateGroupSemanticContext(const DartPlantLiveVmContext& left,
                                            const DartPlantLiveVmContext& right) {
    // Only compare native isolate-group ownership/root-container identity here.
    // Dart's moving GC is allowed to relocate tagged heap objects and updates
    // Thread/ObjectStore roots accordingly. PP/global_object_pool, canonical
    // objects, Function/Code objects, and the cached ClassTable table contents
    // are therefore observation-scoped evidence, not a physical-owner receipt.
    // Treating those addresses as semantic identity spuriously advanced the
    // runtime generation after compaction even though the IsolateGroup owner
    // itself had not changed.
    //
    // `isolate` is deliberately excluded too: multiple Isolates may belong to
    // one IsolateGroup and must project the same group owner. The stable native
    // IsolateGroup/ClassTable/ObjectStore/heap-base tuple still lets us fail
    // closed if a recycled IsolateGroup address is observed with a different
    // root-container incarnation.
    return left.profile_version == right.profile_version &&
           left.isolate_group == right.isolate_group && left.class_table == right.class_table &&
           left.object_store == right.object_store && left.heap_base == right.heap_base;
}

namespace {

bool BuildIdMatches(const ModuleImage& module, const std::string& expected) {
    return expected.empty() || EqualsIgnoreCaseAscii(module.build_id, expected);
}

enum class ModuleSelectionState { kMissing, kUnique, kAmbiguous };

struct ModuleSelection {
    ModuleSelectionState state = ModuleSelectionState::kMissing;
    std::optional<ModuleImage> module;
    uint32_t candidate_count = 0;
};

ModuleSelection SelectProfileModule(const std::vector<ModuleImage>& modules,
                                    const std::string& name, const std::string& build_id,
                                    uintptr_t executable_anchor = 0) {
    const bool exact_path = name.find('/') != std::string::npos;
    ModuleSelection selection;
    for (const auto& module : modules) {
        const bool name_matches = exact_path ? module.path == name : module.name == name;
        if (!name_matches || !BuildIdMatches(module, build_id)) continue;
        if (executable_anchor != 0 && !module.ContainsExecutable(executable_anchor, 1)) continue;
        if (selection.candidate_count != UINT32_MAX) ++selection.candidate_count;
        if (selection.module.has_value()) {
            selection.state = ModuleSelectionState::kAmbiguous;
            selection.module.reset();
            return selection;
        }
        selection.state = ModuleSelectionState::kUnique;
        selection.module = module;
    }
    return selection;
}

bool CanUseLiveVmForQuery(const DartPlantMethodQuery& query) {
    return query.entry_kind >= DARTPLANT_ENTRY_DEFAULT &&
           query.entry_kind <= DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED &&
           query.class_name != nullptr && query.class_name[0] != '\0' &&
           (query.signature == nullptr || query.signature[0] == '\0');
}

bool SameSnapshotLogicalFunction(const SnapshotFunction& left, const SnapshotFunction& right) {
    return left.library_uri == right.library_uri && left.class_name == right.class_name &&
           left.function_name == right.function_name && left.signature == right.signature;
}

bool SameExecutableRange(const ExecutableRange& left, const ExecutableRange& right) {
    return left.start == right.start && left.end == right.end &&
           left.file_offset == right.file_offset && left.virtual_address == right.virtual_address &&
           left.file_size == right.file_size;
}

bool SameModuleIdentity(const std::optional<ModuleImage>& left,
                        const std::optional<ModuleImage>& right) {
    if (left.has_value() != right.has_value()) return false;
    if (!left.has_value()) return true;
    if (left->name != right->name || left->path != right->path ||
        !EqualsIgnoreCaseAscii(left->build_id, right->build_id) ||
        left->load_bias != right->load_bias ||
        left->executable_ranges.size() != right->executable_ranges.size()) {
        return false;
    }
    return std::equal(left->executable_ranges.begin(), left->executable_ranges.end(),
                      right->executable_ranges.begin(), SameExecutableRange);
}

bool ContainsModuleIdentity(const std::vector<ModuleImage>& modules,
                            const std::optional<ModuleImage>& expected) {
    if (!expected.has_value()) return false;
    return std::any_of(modules.begin(), modules.end(), [&expected](const ModuleImage& module) {
        return SameModuleIdentity(expected, std::optional<ModuleImage>(module));
    });
}

bool ModuleMatchesEvent(const ModuleImage& module, const char* module_name) {
    if (module_name == nullptr || module_name[0] == '\0') return false;
    const std::string_view requested(module_name);
    return requested.find('/') == std::string_view::npos ? module.name == requested
                                                         : module.path == requested;
}

bool SameSnapshotIdentity(const std::optional<FlutterSnapshotSource>& left,
                          const std::optional<FlutterSnapshotSource>& right) {
    if (left.has_value() != right.has_value()) return false;
    if (!left.has_value()) return true;
    return left->module_name == right->module_name && left->module_path == right->module_path &&
           EqualsIgnoreCaseAscii(left->module_build_id, right->module_build_id) &&
           left->snapshot_hash == right->snapshot_hash &&
           left->snapshot_features == right->snapshot_features &&
           left->profile_name == right->profile_name &&
           left->isolate_instructions_va == right->isolate_instructions_va &&
           left->isolate_instructions_size == right->isolate_instructions_size &&
           left->isolate_instructions_runtime == right->isolate_instructions_runtime &&
           left->compressed_pointers == right->compressed_pointers &&
           left->deferred_program_hash == right->deferred_program_hash;
}

bool EpochAvailable(uint64_t epoch);

bool IsPristineIsolateGroupOwner(const RuntimeIsolateGroupState& group) {
    return !group.retired && group.incarnation_epoch == 0 && group.isolate_group_identity == 0 &&
           !group.app_module.has_value() && !group.snapshot.has_value() &&
           group.image_set.empty() && !group.live_vm_context.has_value() &&
           !group.live_snapshot_index.has_value() && !group.artifact_snapshot_index.has_value() &&
           group.abi_evidence.empty();
}

bool IsPristineEngineOwner(const RuntimeEngineState& engine) {
    return engine.incarnation_epoch == 0 && engine.anchor == 0 && !engine.module.has_value() &&
           engine.isolate_groups.size() == 1 && engine.isolate_groups[0] != nullptr &&
           IsPristineIsolateGroupOwner(*engine.isolate_groups[0]);
}

void ActivateEngineOwnerForAnchorLockedInternal(DartPlantRuntime* runtime,
                                                const std::vector<ModuleImage>& modules,
                                                uintptr_t engine_anchor) {
    auto& process = RuntimeProcess(runtime);
    const ModuleSelection selection =
        SelectProfileModule(modules, runtime->profile.runtime_module_name,
                            runtime->profile.runtime_build_id, engine_anchor);

    if (selection.state == ModuleSelectionState::kUnique && selection.module.has_value()) {
        for (size_t index = 0; index < process.engines.size(); ++index) {
            auto& engine = *process.engines[index];
            if (engine.retired) continue;
            if (!SameModuleIdentity(engine.module, selection.module)) continue;
            engine.anchor = engine_anchor;
            process.active_engine_index = index;
            ProjectActiveGeneration(runtime);
            ProjectActiveRuntimeState(runtime);
            return;
        }
    }

    for (size_t index = 0; index < process.engines.size(); ++index) {
        auto& engine = *process.engines[index];
        if (engine.retired || engine.anchor != engine_anchor || engine_anchor == 0 ||
            engine.module.has_value()) {
            continue;
        }
        process.active_engine_index = index;
        ProjectActiveGeneration(runtime);
        ProjectActiveRuntimeState(runtime);
        return;
    }

    for (size_t index = 0; index < process.engines.size(); ++index) {
        auto& engine = *process.engines[index];
        if (engine.retired) continue;
        if (!IsPristineEngineOwner(engine)) continue;
        engine.anchor = engine_anchor;
        process.active_engine_index = index;
        ProjectActiveGeneration(runtime);
        ProjectActiveRuntimeState(runtime);
        return;
    }

    auto engine = std::make_unique<RuntimeEngineState>();
    engine->anchor = engine_anchor;
    process.engines.push_back(std::move(engine));
    process.active_engine_index = process.engines.size() - 1;
    ProjectActiveGeneration(runtime);
    ProjectActiveRuntimeState(runtime);
}

void EnsureFreshIsolateGroupOwnerLocked(DartPlantRuntime* runtime) {
    auto& engine = RuntimeEngine(runtime);
    if (!RuntimeIsolateGroup(runtime).retired) return;

    for (size_t index = 0; index < engine.isolate_groups.size(); ++index) {
        const auto& candidate = engine.isolate_groups[index];
        if (candidate == nullptr || candidate->retired ||
            !IsPristineIsolateGroupOwner(*candidate)) {
            continue;
        }
        engine.active_isolate_group_index = index;
        ProjectActiveGeneration(runtime);
        ProjectActiveRuntimeState(runtime);
        return;
    }

    engine.isolate_groups.push_back(std::make_unique<RuntimeIsolateGroupState>());
    engine.active_isolate_group_index = engine.isolate_groups.size() - 1;
    ProjectActiveGeneration(runtime);
    ProjectActiveRuntimeState(runtime);
}

void RestoreOwnerProjectionLocked(DartPlantRuntime* runtime, size_t preferred_engine_index,
                                  const std::vector<size_t>& preferred_group_indices) {
    auto& process = RuntimeProcess(runtime);
    if (process.engines.empty()) return;

    for (size_t engine_index = 0; engine_index < process.engines.size(); ++engine_index) {
        auto& engine = *process.engines[engine_index];
        if (engine.isolate_groups.empty()) continue;
        const size_t preferred_group = engine_index < preferred_group_indices.size()
                                           ? preferred_group_indices[engine_index]
                                           : engine.active_isolate_group_index;
        size_t restore_group = SIZE_MAX;
        if (preferred_group < engine.isolate_groups.size() &&
            engine.isolate_groups[preferred_group] != nullptr &&
            !engine.isolate_groups[preferred_group]->retired) {
            restore_group = preferred_group;
        } else if (engine.active_isolate_group_index < engine.isolate_groups.size() &&
                   engine.isolate_groups[engine.active_isolate_group_index] != nullptr &&
                   !engine.isolate_groups[engine.active_isolate_group_index]->retired) {
            restore_group = engine.active_isolate_group_index;
        } else {
            for (size_t index = 0; index < engine.isolate_groups.size(); ++index) {
                const auto& group = engine.isolate_groups[index];
                if (group != nullptr && !group->retired) {
                    restore_group = index;
                    break;
                }
            }
        }
        if (restore_group == SIZE_MAX) {
            restore_group = std::min(preferred_group, engine.isolate_groups.size() - 1);
        }
        engine.active_isolate_group_index = restore_group;
    }

    size_t restore_engine = SIZE_MAX;
    const auto engine_has_live_group = [&process](size_t index) {
        if (index >= process.engines.size() || process.engines[index] == nullptr ||
            process.engines[index]->retired) {
            return false;
        }
        const auto& engine = *process.engines[index];
        return engine.active_isolate_group_index < engine.isolate_groups.size() &&
               engine.isolate_groups[engine.active_isolate_group_index] != nullptr &&
               !engine.isolate_groups[engine.active_isolate_group_index]->retired;
    };
    if (engine_has_live_group(preferred_engine_index)) {
        restore_engine = preferred_engine_index;
    } else if (engine_has_live_group(process.active_engine_index)) {
        restore_engine = process.active_engine_index;
    } else {
        for (size_t index = 0; index < process.engines.size(); ++index) {
            if (engine_has_live_group(index)) {
                restore_engine = index;
                break;
            }
        }
    }
    if (restore_engine == SIZE_MAX) {
        restore_engine = std::min(preferred_engine_index, process.engines.size() - 1);
    }
    process.active_engine_index = restore_engine;
    ProjectActiveGeneration(runtime);
    ProjectActiveRuntimeState(runtime);
}

DartPlantStatus RetireActiveIsolateGroupOwnerLocked(
    DartPlantRuntime* runtime, const std::vector<ModuleImage>& current_modules) {
    auto& group = RuntimeIsolateGroup(runtime);
    if (group.retired) return DARTPLANT_OK;
    if (group.generation == nullptr ||
        group.generation->load(std::memory_order_acquire) == UINT64_MAX) {
        SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
        SetLastError("isolate-group generation space is exhausted during owner retirement");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    group.generation->fetch_add(1, std::memory_order_acq_rel);
    RuntimeImageSet staged;
    const DartPlantStatus transition_status =
        TransitionRuntimeImages(runtime, std::move(staged), current_modules, true);
    if (transition_status != DARTPLANT_OK) {
        SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
        return transition_status;
    }

    group.live_vm_context.reset();
    group.capability_bindings = {};
    group.live_snapshot_index.reset();
    group.artifact_snapshot_index.reset();
    group.bound_artifact_snapshot_generation = 0;
    group.live_vm_null_value = 0;
    group.live_vm_bool_true_value = 0;
    group.live_vm_bool_false_value = 0;
    group.profile_matched = false;
    group.runtime_state = DARTPLANT_RUNTIME_CREATED;
    group.retired = true;
    ProjectActiveRuntimeState(runtime);
    return DARTPLANT_OK;
}

DartPlantStatus RetireActiveEngineOwnerLocked(DartPlantRuntime* runtime,
                                              const std::vector<ModuleImage>& current_modules) {
    auto& engine = RuntimeEngine(runtime);
    if (engine.retired) return DARTPLANT_OK;

    const size_t saved_group_index = engine.active_isolate_group_index;
    DartPlantStatus aggregate = DARTPLANT_OK;
    std::string first_error;
    for (size_t group_index = 0; group_index < engine.isolate_groups.size(); ++group_index) {
        const auto& group = engine.isolate_groups[group_index];
        if (group == nullptr || group->retired) continue;
        engine.active_isolate_group_index = group_index;
        ProjectActiveGeneration(runtime);
        ProjectActiveRuntimeState(runtime);
        const DartPlantStatus status =
            RetireActiveIsolateGroupOwnerLocked(runtime, current_modules);
        if (status != DARTPLANT_OK && aggregate == DARTPLANT_OK) {
            aggregate = status;
            first_error = LastError();
        }
    }
    engine.active_isolate_group_index =
        std::min(saved_group_index,
                 engine.isolate_groups.empty() ? size_t{0} : engine.isolate_groups.size() - 1);
    ProjectActiveGeneration(runtime);
    ProjectActiveRuntimeState(runtime);
    if (aggregate != DARTPLANT_OK) {
        SetLastError(first_error.empty() ? "engine owner retirement failed" : first_error);
        return aggregate;
    }
    engine.retired = true;
    return DARTPLANT_OK;
}

DartPlantStatus ActivateIsolateGroupOwnerForContextLockedInternal(
    DartPlantRuntime* runtime, const DartPlantLiveVmContext& context,
    const FlutterSnapshotSource& snapshot) {
    if (context.isolate_group == 0) {
        SetLastError("live VM context has no isolate-group identity");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    auto& process = RuntimeProcess(runtime);
    auto& engine = RuntimeEngine(runtime);
    auto& active = RuntimeIsolateGroup(runtime);
    // Every isolate group under one physical Flutter engine executes the same
    // app/runtime image set. Preserve that physical-image seed before a stale
    // same-address owner is retired; retirement intentionally clears the old
    // group's semantic image bindings, but the replacement incarnation still
    // needs a pristine copy of the mapped app/deferred descriptors.
    const std::optional<ModuleImage> owner_seed_app = active.app_module;
    const std::optional<FlutterSnapshotSource> owner_seed_snapshot = active.snapshot;
    const RuntimeImageSet owner_seed_images = active.image_set;
    const bool owner_seed_profile_matched = active.profile_matched;
    if (!active.retired && (active.isolate_group_identity == 0 ||
                            active.isolate_group_identity == context.isolate_group)) {
        if (active.isolate_group_identity == 0 || !active.live_vm_context.has_value() ||
            SameRuntimeIsolateGroupSemanticContext(*active.live_vm_context, context)) {
            return DARTPLANT_OK;
        }
        // Dart's native IsolateGroup allocation can reuse an address after an
        // engine/root-isolate is destroyed. The raw pointer is therefore not a
        // sufficient physical-incarnation receipt. A matching address with a
        // different stable root-container tuple is a new owner: drain/retire
        // the old epoch before publishing the replacement.
        const DartPlantStatus retire_status =
            RetireActiveIsolateGroupOwnerLocked(runtime, process.modules);
        if (retire_status != DARTPLANT_OK) return retire_status;
    }

    for (size_t index = 0; index < engine.isolate_groups.size(); ++index) {
        const auto& candidate = engine.isolate_groups[index];
        if (candidate == nullptr || candidate->retired ||
            candidate->isolate_group_identity != context.isolate_group) {
            continue;
        }
        if (candidate->live_vm_context.has_value() &&
            !SameRuntimeIsolateGroupSemanticContext(*candidate->live_vm_context, context)) {
            engine.active_isolate_group_index = index;
            ProjectActiveGeneration(runtime);
            ProjectActiveRuntimeState(runtime);
            const DartPlantStatus retire_status =
                RetireActiveIsolateGroupOwnerLocked(runtime, process.modules);
            if (retire_status != DARTPLANT_OK) return retire_status;
            continue;
        }
        engine.active_isolate_group_index = index;
        ProjectActiveGeneration(runtime);
        ProjectActiveRuntimeState(runtime);
        return DARTPLANT_OK;
    }

    if (!EpochAvailable(process.next_isolate_group_incarnation_epoch)) {
        SetLastError("Dart isolate-group incarnation epoch space is exhausted");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    auto group = std::make_unique<RuntimeIsolateGroupState>();
    group->incarnation_epoch = process.next_isolate_group_incarnation_epoch++;
    group->isolate_group_identity = context.isolate_group;
    group->isolate_generation = 1;
    group->app_module = owner_seed_app;
    group->snapshot = owner_seed_snapshot.has_value()
                          ? owner_seed_snapshot
                          : std::optional<FlutterSnapshotSource>(snapshot);
    // Module-profile matching is a physical app/runtime-image fact shared by
    // every isolate group under this engine. Do not make a freshly discovered
    // group re-prove loader identity before it can perform its first live VM
    // bootstrap. Live Function indexes, capability bindings, and READY state
    // remain per-owner and are intentionally not inherited.
    group->profile_matched = owner_seed_profile_matched;
    if (!owner_seed_images.empty()) {
        group->image_set = owner_seed_images;
        group->image_set.ResetSemanticBindings();
        group->image_set.BindGeneration(group->generation->load(std::memory_order_acquire));
        group->image_set.BindOwnerEpochs(engine.incarnation_epoch, group->incarnation_epoch);
        group->image_set.ActivateAll();
    }
    engine.isolate_groups.push_back(std::move(group));
    engine.active_isolate_group_index = engine.isolate_groups.size() - 1;
    ProjectActiveGeneration(runtime);
    ProjectActiveRuntimeState(runtime);
    return DARTPLANT_OK;
}

bool EpochAvailable(uint64_t epoch) { return epoch != 0 && epoch != UINT64_MAX; }

std::optional<RuntimeImageSet> BuildRuntimeImageSet(
    const std::vector<ModuleImage>& modules, const std::optional<ModuleImage>& root_module,
    const std::optional<FlutterSnapshotSource>& root_snapshot, uint64_t runtime_generation,
    std::string* error) {
    RuntimeImageSet result;
    if (!root_module.has_value() || !root_snapshot.has_value()) return result;
    if (!result.SetRoot(*root_module, *root_snapshot, runtime_generation, error)) {
        return std::nullopt;
    }

    for (const auto& module : modules) {
        const auto unit_id = ParseDeferredLoadingUnitId(root_module->name, module.name);
        if (!unit_id.has_value()) continue;
        std::string deferred_error;
        const auto snapshot = DiscoverDeferredFlutterSnapshot(module, &deferred_error);
        if (!snapshot.has_value()) {
            if (error != nullptr) {
                *error = deferred_error.empty()
                             ? "deferred loading-unit module has no usable Dart snapshot source"
                             : deferred_error;
            }
            return std::nullopt;
        }
        if (!result.AddDeferred(module, *snapshot, *unit_id, runtime_generation, error)) {
            return std::nullopt;
        }
    }
    return result;
}

std::mutex& RuntimeRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<RuntimeRegistration>>& RuntimeRegistry() {
    static std::vector<std::shared_ptr<RuntimeRegistration>> runtimes;
    return runtimes;
}

struct RuntimeRefreshWorkerState {
    std::mutex mutex;
    std::condition_variable completed;
    std::atomic_uint64_t requested_epoch{0};
    uint64_t completed_epoch = 0;
    struct RefreshResult {
        uint64_t first_epoch = 0;
        uint64_t last_epoch = 0;
        DartPlantStatus status = DARTPLANT_OK;
    };
    std::deque<RefreshResult> results;
    std::atomic<RuntimeModuleRefreshReporter> reporter{nullptr};
    std::atomic_bool started{false};
};

RuntimeRefreshWorkerState& RuntimeRefreshWorker() {
    static auto* state = new RuntimeRefreshWorkerState;
    return *state;
}

void RegisterRuntime(DartPlantRuntime* runtime) {
    std::lock_guard lock(RuntimeRegistryMutex());
    auto registration = std::make_shared<RuntimeRegistration>();
    registration->runtime = runtime;
    registration->app_module_name = runtime->profile.app_module_name;
    registration->runtime_module_name = runtime->profile.runtime_module_name;
    RuntimeRegistry().push_back(std::move(registration));
}

std::shared_ptr<RuntimeRegistration> CloseRuntime(DartPlantRuntime* runtime) {
    std::unique_lock lock(RuntimeRegistryMutex());
    const auto found = std::find_if(
        RuntimeRegistry().begin(), RuntimeRegistry().end(),
        [runtime](const auto& registration) { return registration->runtime == runtime; });
    if (found == RuntimeRegistry().end()) return nullptr;
    auto registration = *found;
    registration->closing.store(true, std::memory_order_release);
    RuntimeRegistry().erase(found);
    registration->idle.wait(lock, [&registration] { return registration->active_operations == 0; });
    return registration;
}

std::vector<RuntimeOperationLease> PinRuntimeRefreshes() {
    std::vector<RuntimeOperationLease> operations;
    std::lock_guard lock(RuntimeRegistryMutex());
    for (const auto& registration : RuntimeRegistry()) {
        if (registration->closing.load(std::memory_order_acquire)) continue;
        ++registration->active_operations;
        RuntimeOperationLease lease;
        lease.registration = registration;
        operations.push_back(std::move(lease));
    }
    return operations;
}

void ReleaseRuntimeOperation(std::shared_ptr<RuntimeRegistration>* registration) {
    if (registration == nullptr || *registration == nullptr) return;
    std::lock_guard lock(RuntimeRegistryMutex());
    auto current = std::move(*registration);
    --current->active_operations;
    if (current->closing && current->active_operations == 0) current->idle.notify_all();
}

DartPlantStatus ResolveLiveIndexedRuntimeMethod(
    const SnapshotIndex& index, const RuntimeImageSet* image_set, const ModuleImage& root_module,
    DartEntryTargetRegistry& entry_targets, const DartPlantMethodQuery& query,
    const std::shared_ptr<std::atomic_uint64_t>& runtime_generation,
    uint64_t expected_runtime_generation, DartPlantMethod** out_method) {
    bool ambiguous = false;
    const SnapshotFunction* record = index.FindSnapshotFunction(
        query.library_uri, query.class_name == nullptr ? "" : query.class_name, query.function_name,
        query.signature == nullptr ? "" : query.signature, query.entry_kind, &ambiguous);
    if (ambiguous) {
        SetLastError("Dart method identity is ambiguous in the live Function index");
        return DARTPLANT_AMBIGUOUS_METHOD;
    }
    if (record == nullptr || !record->live || record->runtime_entry == 0 ||
        record->function_object == 0 || record->code_object == 0) {
        SetLastError("Dart method was not found in the live Function index");
        return DARTPLANT_METHOD_NOT_FOUND;
    }
    if (root_module.name != index.module_name ||
        (!index.module_path.empty() && root_module.path != index.module_path) ||
        !BuildIdMatches(root_module, index.build_id)) {
        SetLastError("live Function index module identity does not match the selected app image");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const RuntimeImage* owning_image = nullptr;
    const ModuleImage* module = &root_module;
    uint64_t image_id = record->runtime_image_id;
    if (image_id != kInvalidRuntimeImageId) {
        if (image_set == nullptr || (owning_image = image_set->FindById(image_id)) == nullptr) {
            SetLastError("live Function index references an unknown runtime image");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        if (record->loading_unit_id != 0 &&
            record->loading_unit_id != owning_image->loading_unit_id) {
            SetLastError("live Function loading-unit identity disagrees with runtime image");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        if (owning_image->runtime_generation != expected_runtime_generation ||
            !owning_image->IsActive() || owning_image->live_entry_count == 0 ||
            record->runtime_image_incarnation_epoch != owning_image->incarnation_epoch ||
            record->engine_incarnation_epoch != owning_image->engine_incarnation_epoch ||
            record->isolate_group_incarnation_epoch !=
                owning_image->isolate_group_incarnation_epoch ||
            record->runtime_generation != owning_image->runtime_generation) {
            SetLastError(
                "live Function runtime image is not semantically bound to this runtime generation");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        module = &owning_image->module;
    }
    if (!module->ContainsExecutable(record->runtime_entry, record->code_size)) {
        SetLastError("live Function index entry is outside executable module ranges");
        return DARTPLANT_ADDRESS_OUTSIDE_EXECUTABLE;
    }

    MethodRecord method_record;
    method_record.library_uri = record->library_uri;
    method_record.class_name = record->class_name;
    method_record.function_name = record->function_name;
    method_record.signature = record->signature;
    method_record.entry_kind = record->entry_kind;
    method_record.address_kind = DARTPLANT_ADDRESS_RUNTIME;
    method_record.address = record->runtime_entry;
    method_record.code_size = static_cast<uint32_t>(record->code_size);

    DartRuntimeOwnerIdentity owner{};
    if (owning_image != nullptr) {
        owner = owning_image->OwnerIdentity();
    } else {
        owner.runtime_generation = expected_runtime_generation;
        owner.image_id = image_id;
    }
    auto code_target = entry_targets.GetOrCreate(
        record->runtime_entry, static_cast<uint32_t>(record->code_size), record->code_object,
        record->entry_alias_count, DARTPLANT_CODE_IDENTITY_UNKNOWN,
        static_cast<uintptr_t>(record->code_payload_start), record->code_instructions_length,
        image_id, owner);
    if (code_target == nullptr) {
        SetLastError("live Function index produced an invalid entry target");
        return DARTPLANT_METHOD_NOT_FOUND;
    }
    const RuntimeProfileRecord* profile = FindRuntimeProfileByVersion(index.vm_profile_version);
    if (index.vm_profile_version != 0 && profile == nullptr) {
        SetLastError("live Function index has no selected candidate ABI profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    auto function = std::make_shared<DartFunctionHandle>();
    function->identity = MethodIdentityFromRecord(method_record);
    function->image_id = image_id;
    function->owner = owner;
    function->function_object = record->function_object;
    function->code_object = record->code_object;
    function->source = DartFunctionSource::kLiveVm;
    function->function_kind = record->function_kind;
    function->runtime_profile_version = profile == nullptr ? 0 : profile->live_vm.profile_version;
    function->live_observation_receipt = true;
    function->closure_call_entry_only = record->closure_call_entry_only;
    function->thread_jump_to_frame_entry_point_offset =
        profile == nullptr ? 0 : profile->thread_jump_to_frame_entry_point_offset;
    function->code_target = code_target;
    code_target->AddAlias(function->identity);

    auto* method = new DartPlantMethod;
    method->record = std::move(method_record);
    method->module = *module;
    method->function = std::move(function);
    method->runtime_generation = runtime_generation;
    method->expected_runtime_generation = expected_runtime_generation;
    *out_method = method;
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus ResolveArtifactIndexedRuntimeMethod(
    const SnapshotIndex& index, const FlutterSnapshotSource& snapshot, const ModuleImage& module,
    const RuntimeImage* runtime_image, DartEntryTargetRegistry& entry_targets,
    const DartPlantMethodQuery& query,
    const std::shared_ptr<std::atomic_uint64_t>& runtime_generation,
    uint64_t expected_runtime_generation, DartPlantMethod** out_method) {
    bool ambiguous = false;
    const SnapshotFunction* record = index.FindSnapshotFunction(
        query.library_uri, query.class_name == nullptr ? "" : query.class_name, query.function_name,
        query.signature == nullptr ? "" : query.signature, query.entry_kind, &ambiguous);
    if (ambiguous) {
        SetLastError("Dart method identity is ambiguous in the artifact snapshot index");
        return DARTPLANT_AMBIGUOUS_METHOD;
    }
    if (record == nullptr) {
        SetLastError("Dart method was not found in the artifact snapshot index");
        return DARTPLANT_METHOD_NOT_FOUND;
    }
    if (record->runtime_entry == 0 || record->code_size == 0 || record->fingerprint.empty()) {
        SetLastError("artifact snapshot index record is not runtime-bound");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (!snapshot.Matches(module) || module.name != index.module_name ||
        !BuildIdMatches(module, index.build_id) || snapshot.snapshot_hash != index.snapshot_hash) {
        SetLastError("artifact snapshot index identity does not match the selected app image");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (!module.ContainsExecutable(record->runtime_entry, record->code_size)) {
        SetLastError("artifact snapshot index entry is outside executable module ranges");
        return DARTPLANT_ADDRESS_OUTSIDE_EXECUTABLE;
    }
    if (FingerprintCodeWithManagedPatches(reinterpret_cast<const void*>(record->runtime_entry),
                                          record->code_size) != record->fingerprint) {
        SetLastError("artifact snapshot index code fingerprint no longer matches the live image");
        return DARTPLANT_FINGERPRINT_MISMATCH;
    }

    MethodRecord method_record;
    method_record.library_uri = record->library_uri;
    method_record.class_name = record->class_name;
    method_record.function_name = record->function_name;
    method_record.signature = record->signature;
    method_record.entry_kind = record->entry_kind;
    method_record.address_kind = DARTPLANT_ADDRESS_RUNTIME;
    method_record.address = record->runtime_entry;
    method_record.code_size = static_cast<uint32_t>(record->code_size);
    method_record.fingerprint = record->fingerprint;

    const uint64_t image_id = runtime_image == nullptr ? 0 : runtime_image->id;
    DartRuntimeOwnerIdentity owner{};
    if (runtime_image != nullptr) {
        if (!runtime_image->IsActive() ||
            runtime_image->runtime_generation != expected_runtime_generation) {
            SetLastError("artifact runtime image is not active for this runtime generation");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        owner = runtime_image->OwnerIdentity();
    } else {
        owner.runtime_generation = expected_runtime_generation;
    }
    auto code_target = entry_targets.GetOrCreate(
        record->runtime_entry, static_cast<uint32_t>(record->code_size), 0,
        std::max<uint32_t>(1, record->entry_alias_count), record->code_identity_proof,
        static_cast<uintptr_t>(record->code_payload_start), record->code_instructions_length,
        image_id, owner);
    if (code_target == nullptr) {
        SetLastError("artifact snapshot index produced an invalid entry target");
        return DARTPLANT_METHOD_NOT_FOUND;
    }
    auto function = std::make_shared<DartFunctionHandle>();
    function->identity = MethodIdentityFromRecord(method_record);
    function->image_id = image_id;
    function->owner = owner;
    function->source = DartFunctionSource::kOfflineSnapshotIndex;
    function->function_kind = record->function_kind;
    if (const RuntimeProfileRecord* profile =
            FindRuntimeProfileBySnapshot(index.snapshot_hash, snapshot.profile_name);
        profile != nullptr) {
        function->runtime_profile_version = profile->live_vm.profile_version;
    }
    function->closure_call_entry_only = record->closure_call_entry_only;
    function->thread_jump_to_frame_entry_point_offset =
        ThreadJumpToFrameOffsetForSnapshot(index.snapshot_hash, snapshot.profile_name);
    function->code_target = code_target;
    code_target->AddAlias(function->identity);

    auto* method = new DartPlantMethod;
    method->record = std::move(method_record);
    method->module = module;
    method->function = std::move(function);
    method->runtime_generation = runtime_generation;
    method->expected_runtime_generation = expected_runtime_generation;
    *out_method = method;
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus MergeValidatedArtifactIdentityIntoLiveMethod(const SnapshotIndex& artifact_index,
                                                             const DartPlantMethodQuery& query,
                                                             DartPlantMethod* live_method) {
    if (live_method == nullptr || live_method->function == nullptr ||
        live_method->function->code_target == nullptr ||
        live_method->function->source != DartFunctionSource::kLiveVm) {
        SetLastError("live method is unavailable for artifact identity convergence");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    bool ambiguous = false;
    const SnapshotFunction* artifact = artifact_index.FindSnapshotFunction(
        query.library_uri, query.class_name == nullptr ? "" : query.class_name, query.function_name,
        query.signature == nullptr ? "" : query.signature, query.entry_kind, &ambiguous);
    if (ambiguous) {
        SetLastError("artifact identity is ambiguous for the resolved live method");
        return DARTPLANT_AMBIGUOUS_METHOD;
    }
    // Most live Functions do not have a compiler sidecar. Their VM identity
    // remains usable, but no artifact Code-identity claim is synthesized.
    if (artifact == nullptr) return DARTPLANT_OK;

    const uintptr_t live_entry = MethodTarget(live_method);
    const uint32_t live_code_size = MethodCodeSize(live_method);
    if (artifact->runtime_entry == 0 || artifact->code_size == 0 ||
        artifact->runtime_entry != live_entry || artifact->code_size != live_code_size) {
        SetLastError("artifact and live Function indices disagree on the physical Code entry");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    const auto& target = live_method->function->code_target;
    const uint32_t artifact_alias_count = std::max<uint32_t>(1, artifact->entry_alias_count);
    if (artifact->code_identity_proof == DARTPLANT_CODE_IDENTITY_UNIQUE &&
        (artifact_alias_count != 1 || target->AliasCount() > 1)) {
        SetLastError("artifact UNIQUE Code identity conflicts with live alias evidence");
        return DARTPLANT_METADATA_INVALID;
    }
    if (!target->MergeEvidence(live_code_size, 0, artifact_alias_count,
                               artifact->code_identity_proof)) {
        SetLastError("artifact Code identity conflicts with the resolved live target");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus BindArtifactSnapshotIndex(SnapshotIndex* index,
                                          const FlutterSnapshotSource& snapshot,
                                          const ModuleImage& module,
                                          const SnapshotIndex* existing = nullptr) {
    if (index == nullptr || index->functions.empty() || index->module_name != module.name ||
        index->build_id.empty() || !EqualsIgnoreCaseAscii(index->build_id, module.build_id) ||
        index->snapshot_hash.empty() || index->snapshot_hash != snapshot.snapshot_hash ||
        !snapshot.Matches(module)) {
        SetLastError("artifact snapshot index header does not match the live app image");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    const RuntimeProfileRecord* vm_profile =
        FindRuntimeProfileBySnapshot(index->snapshot_hash, snapshot.profile_name);

    std::unordered_map<uintptr_t, DartPlantCodeIdentityProof> identity_proofs;
    std::unordered_map<uintptr_t, uint32_t> physical_alias_counts;
    for (auto& record : index->functions) {
        if (record.entry_kind < DARTPLANT_ENTRY_DEFAULT ||
            record.entry_kind > DARTPLANT_ENTRY_MONOMORPHIC_UNCHECKED || record.entry_va == 0 ||
            record.code_size == 0 || record.code_size > UINT32_MAX || record.fingerprint.empty()) {
            SetLastError("artifact snapshot index contains an unsupported or incomplete record");
            return DARTPLANT_METADATA_INVALID;
        }
        bool closure_kind = false;
        if (vm_profile != nullptr) {
            closure_kind =
                IsClosureFunctionKind(vm_profile->live_vm.profile_version, record.function_kind);
        } else if (record.function_kind != 0 || record.closure_call_entry_only) {
            // Ordinary V1/V2 artifacts and synthetic tests do not require VM
            // private Function-kind knowledge. Any artifact that claims a
            // non-regular kind, however, needs an exact profile so closure
            // entry/stack semantics can be source-verified rather than guessed.
            SetLastError("artifact Function kind requires an exact Dart VM runtime profile");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        if (record.closure_call_entry_only != closure_kind ||
            (closure_kind && record.entry_kind != DARTPLANT_ENTRY_DEFAULT)) {
            SetLastError("artifact closure entry evidence contradicts Dart PRODUCT AOT semantics");
            return DARTPLANT_METADATA_INVALID;
        }
        const SnapshotFunction* reusable = nullptr;
        if (existing != nullptr && existing->module_name == index->module_name &&
            existing->build_id == index->build_id &&
            existing->snapshot_hash == index->snapshot_hash) {
            const auto found = std::find_if(
                existing->functions.begin(), existing->functions.end(), [&record](const auto& old) {
                    return old.entry_va == record.entry_va && old.code_size == record.code_size &&
                           old.code_section_va == record.code_section_va &&
                           old.code_payload_va == record.code_payload_va &&
                           old.code_instructions_length == record.code_instructions_length &&
                           old.fingerprint == record.fingerprint && old.runtime_entry != 0;
                });
            if (found != existing->functions.end()) reusable = &*found;
        }
        uintptr_t runtime_entry = 0;
        if (reusable != nullptr) {
            runtime_entry = reusable->runtime_entry;
            if (!module.ContainsExecutable(runtime_entry, static_cast<size_t>(record.code_size))) {
                SetLastError("previously bound artifact entry is no longer executable");
                return DARTPLANT_ADDRESS_OUTSIDE_EXECUTABLE;
            }
        } else {
            const auto resolved = snapshot.ResolveInstructionVa(module, record.entry_va);
            if (!resolved.has_value() ||
                !module.ContainsExecutable(*resolved, static_cast<size_t>(record.code_size))) {
                SetLastError("artifact snapshot index entry cannot be mapped into the live image");
                return DARTPLANT_ADDRESS_OUTSIDE_EXECUTABLE;
            }
            runtime_entry = *resolved;
            if (FingerprintCodeWithManagedPatches(reinterpret_cast<const void*>(runtime_entry),
                                                  record.code_size) != record.fingerprint) {
                SetLastError(
                    "artifact snapshot index code fingerprint does not match the live image");
                return DARTPLANT_FINGERPRINT_MISMATCH;
            }
        }
        record.runtime_entry = runtime_entry;
        record.code_entry = runtime_entry;
        if (record.code_payload_va != 0) {
            const auto resolved_payload =
                snapshot.ResolveInstructionVa(module, record.code_payload_va);
            if (!resolved_payload.has_value() || record.code_instructions_length == 0 ||
                !module.ContainsExecutable(*resolved_payload, record.code_instructions_length) ||
                runtime_entry < *resolved_payload ||
                runtime_entry - *resolved_payload >= record.code_instructions_length ||
                record.code_size !=
                    record.code_instructions_length - (runtime_entry - *resolved_payload)) {
                SetLastError("artifact Code payload identity does not match the live image");
                return DARTPLANT_PROFILE_MISMATCH;
            }
            record.code_payload_start = *resolved_payload;
        } else {
            // Compatibility with V1/V2 sidecars: without explicit payload
            // identity the selected entry is conservatively its own payload.
            record.code_payload_start = runtime_entry;
            record.code_instructions_length = static_cast<uint32_t>(record.code_size);
        }

        const auto proof = identity_proofs.find(runtime_entry);
        if (proof == identity_proofs.end()) {
            identity_proofs.emplace(runtime_entry, record.code_identity_proof);
            physical_alias_counts.emplace(runtime_entry, record.physical_entry_alias_count);
        } else if (proof->second != record.code_identity_proof ||
                   physical_alias_counts[runtime_entry] != record.physical_entry_alias_count) {
            SetLastError(
                "artifact snapshot index has inconsistent compiler identity proof for one Code entry");
            return DARTPLANT_METADATA_INVALID;
        }
    }
    for (auto& record : index->functions) {
        uint32_t indexed_alias_count = 0;
        for (size_t candidate_index = 0; candidate_index < index->functions.size();
             ++candidate_index) {
            const auto& candidate = index->functions[candidate_index];
            if (candidate.runtime_entry != record.runtime_entry) continue;
            bool already_counted = false;
            for (size_t earlier = 0; earlier < candidate_index; ++earlier) {
                const auto& previous = index->functions[earlier];
                if (previous.runtime_entry == record.runtime_entry &&
                    SameSnapshotLogicalFunction(previous, candidate)) {
                    already_counted = true;
                    break;
                }
            }
            if (!already_counted) ++indexed_alias_count;
        }
        if (record.code_identity_proof == DARTPLANT_CODE_IDENTITY_UNIQUE &&
            indexed_alias_count != 1) {
            SetLastError("artifact snapshot index contradicts compiler UNIQUE Code identity proof");
            return DARTPLANT_METADATA_INVALID;
        }
        if (record.code_identity_proof == DARTPLANT_CODE_IDENTITY_SHARED &&
            record.physical_entry_alias_count < indexed_alias_count) {
            SetLastError(
                "artifact snapshot index contains more aliases than the compiler identity proof");
            return DARTPLANT_METADATA_INVALID;
        }
        record.entry_alias_count = std::max<uint32_t>(
            indexed_alias_count,
            record.physical_entry_alias_count == 0 ? 1 : record.physical_entry_alias_count);
    }
    ClearLastError();
    return DARTPLANT_OK;
}

}  // namespace

void ActivateRuntimeEngineOwnerForAnchorLocked(DartPlantRuntime* runtime,
                                               const std::vector<ModuleImage>& modules,
                                               uintptr_t engine_anchor) {
    ActivateEngineOwnerForAnchorLockedInternal(runtime, modules, engine_anchor);
}

DartPlantStatus ActivateRuntimeIsolateGroupOwnerForContextLocked(
    DartPlantRuntime* runtime, const DartPlantLiveVmContext& context,
    const FlutterSnapshotSource& snapshot) {
    return ActivateIsolateGroupOwnerForContextLockedInternal(runtime, context, snapshot);
}

bool EqualsIgnoreCaseAscii(const std::string& left, const std::string& right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
                                                     [](unsigned char a, unsigned char b) {
                                                         return std::tolower(a) == std::tolower(b);
                                                     });
}

bool IsCurrentRuntimeMethod(const DartPlantRuntime* runtime, const DartPlantMethod* method) {
    if (runtime == nullptr || method == nullptr ||
        method->runtime_generation != runtime->generation ||
        method->expected_runtime_generation !=
            runtime->generation->load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard lock(runtime->mutex);
    if (dartplant::RuntimeIsolateGroup(runtime).image_set.empty()) return true;
    if (method->function == nullptr || method->function->image_id == 0) return false;
    const RuntimeImage* image =
        dartplant::RuntimeIsolateGroup(runtime).image_set.FindById(method->function->image_id);
    return image != nullptr && image->IsActive() &&
           method->function->owner == image->OwnerIdentity() &&
           SameModuleIdentity(std::optional<ModuleImage>(image->module),
                              std::optional<ModuleImage>(method->module));
}

bool IsRuntimeMethodOwnerAlive(const DartPlantRuntime* runtime, const DartPlantMethod* method) {
    if (runtime == nullptr || method == nullptr || method->function == nullptr ||
        method->function->image_id == 0 || method->runtime_generation == nullptr) {
        return false;
    }
    std::lock_guard lock(runtime->mutex);
    for (const auto& engine_ptr : RuntimeProcess(runtime).engines) {
        if (engine_ptr == nullptr || engine_ptr->retired) continue;
        const auto& engine = *engine_ptr;
        for (const auto& group_ptr : engine.isolate_groups) {
            if (group_ptr == nullptr || group_ptr->retired ||
                group_ptr->generation != method->runtime_generation ||
                group_ptr->generation->load(std::memory_order_acquire) !=
                    method->expected_runtime_generation) {
                continue;
            }
            const RuntimeImage* image = group_ptr->image_set.FindById(method->function->image_id);
            if (image == nullptr || !image->IsActive() ||
                method->function->owner != image->OwnerIdentity()) {
                continue;
            }
            if (!SameModuleIdentity(std::optional<ModuleImage>(image->module),
                                    std::optional<ModuleImage>(method->module))) {
                continue;
            }
            return true;
        }
    }
    return false;
}

DartPlantStatus ReplaceRuntimeArtifactSnapshotIndex(DartPlantRuntime* runtime,
                                                    const DartPlantSnapshotIndexInfo* source,
                                                    uint64_t registry_generation) {
    if (runtime == nullptr || source == nullptr || source->struct_size < sizeof(*source) ||
        registry_generation == 0) {
        SetLastError("registered artifact snapshot index arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = AcquireRuntimeOperation(runtime);
    if (!operation) {
        SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    std::string error;
    auto index = BuildSnapshotIndex(*source, &error);
    if (!index.has_value()) {
        SetLastError(error.empty() ? "registered artifact snapshot index is invalid" : error);
        return DARTPLANT_METADATA_INVALID;
    }

    std::lock_guard lock(runtime->mutex);
    if (!runtime->profile_matched ||
        !dartplant::RuntimeIsolateGroup(runtime).snapshot.has_value() ||
        !dartplant::RuntimeIsolateGroup(runtime).app_module.has_value()) {
        SetLastError("runtime image is not ready for registered artifact binding");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const DartPlantStatus status = BindArtifactSnapshotIndex(
        &*index, *dartplant::RuntimeIsolateGroup(runtime).snapshot,
        *dartplant::RuntimeIsolateGroup(runtime).app_module,
        dartplant::RuntimeIsolateGroup(runtime).artifact_snapshot_index.has_value()
            ? &*dartplant::RuntimeIsolateGroup(runtime).artifact_snapshot_index
            : nullptr);
    if (status != DARTPLANT_OK) return status;
    dartplant::RuntimeIsolateGroup(runtime).artifact_snapshot_index = std::move(index);
    dartplant::RuntimeIsolateGroup(runtime).bound_artifact_snapshot_generation =
        registry_generation;
    ClearLastError();
    return DARTPLANT_OK;
}

RuntimeOperationLease::RuntimeOperationLease(RuntimeOperationLease&& other) noexcept
    : registration(std::move(other.registration)) {}

RuntimeOperationLease& RuntimeOperationLease::operator=(RuntimeOperationLease&& other) noexcept {
    if (this != &other) {
        ReleaseRuntimeOperation(&registration);
        registration = std::move(other.registration);
    }
    return *this;
}

RuntimeOperationLease::~RuntimeOperationLease() { ReleaseRuntimeOperation(&registration); }

RuntimeOperationLease AcquireRuntimeOperation(const DartPlantRuntime* runtime) {
    RuntimeOperationLease lease;
    if (runtime == nullptr) return lease;
    std::lock_guard lock(RuntimeRegistryMutex());
    const auto found = std::find_if(
        RuntimeRegistry().begin(), RuntimeRegistry().end(), [runtime](const auto& registration) {
            return registration->runtime == runtime &&
                   !registration->closing.load(std::memory_order_acquire);
        });
    if (found == RuntimeRegistry().end()) return lease;
    ++(*found)->active_operations;
    lease.registration = *found;
    return lease;
}

size_t RuntimeActiveOperationCountForTesting(const DartPlantRuntime* runtime) {
    if (runtime == nullptr) return 0;
    std::lock_guard lock(RuntimeRegistryMutex());
    const auto found = std::find_if(
        RuntimeRegistry().begin(), RuntimeRegistry().end(),
        [runtime](const auto& registration) { return registration->runtime == runtime; });
    return found == RuntimeRegistry().end() ? 0 : (*found)->active_operations;
}

void SetRuntimeDiagnostics(DartPlantRuntime* runtime, DartPlantResolveStage stage,
                           DartPlantResolveOutcome outcome, DartPlantStatus status,
                           DartPlantResolveRejectReason reject_reason) {
    if (runtime == nullptr) return;
    std::lock_guard lock(runtime->mutex);
    auto& diagnostics = RuntimeDiagnostics(runtime);
    if (stage == DARTPLANT_RESOLVE_MODULE_SELECTION) {
        diagnostics.requested_entry_kind = DARTPLANT_ENTRY_DEFAULT;
        diagnostics.module_candidate_count = 0;
        diagnostics.function_candidate_count = 0;
        diagnostics.code_alias_count = 0;
        diagnostics.abi_provider_count = 0;
        diagnostics.structural_candidate_count = 0;
        diagnostics.structural_relation_count = 0;
        diagnostics.selected_entry = 0;
    } else if (stage == DARTPLANT_RESOLVE_FUNCTION_IDENTITY &&
               outcome == DARTPLANT_RESOLVE_IN_PROGRESS) {
        diagnostics.function_candidate_count = 0;
        diagnostics.code_alias_count = 0;
        diagnostics.abi_provider_count = 0;
        diagnostics.structural_candidate_count = 0;
        diagnostics.structural_relation_count = 0;
        diagnostics.selected_entry = 0;
    } else if (stage == DARTPLANT_RESOLVE_ABI_EVIDENCE &&
               outcome == DARTPLANT_RESOLVE_IN_PROGRESS) {
        diagnostics.abi_provider_count = 0;
    }
    diagnostics.struct_size = sizeof(diagnostics);
    diagnostics.stage = stage;
    diagnostics.outcome = outcome;
    diagnostics.reject_reason = reject_reason;
    diagnostics.status = status;
    diagnostics.runtime_generation = runtime->generation->load(std::memory_order_acquire);
    ProjectActiveDiagnostics(runtime);
}

DartPlantStatus RefreshRuntimeModules(DartPlantRuntime* runtime,
                                      const std::vector<ModuleImage>& modules) {
    if (runtime == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    std::lock_guard lock(runtime->mutex);

    if (RuntimeEngine(runtime).retired) {
        const uintptr_t anchor = RuntimeEngine(runtime).anchor;
        const ModuleSelection replacement =
            SelectProfileModule(modules, runtime->profile.runtime_module_name,
                                runtime->profile.runtime_build_id, anchor);
        if (replacement.state != ModuleSelectionState::kUnique || !replacement.module.has_value()) {
            RuntimeProcess(runtime).modules = modules;
            SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
            SetLastError("retired Flutter engine owner has no unique replacement mapping");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        ActivateEngineOwnerForAnchorLockedInternal(runtime, modules, anchor);
    }
    EnsureFreshIsolateGroupOwnerLocked(runtime);

    SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_MODULE_SELECTION,
                          DARTPLANT_RESOLVE_IN_PROGRESS, DARTPLANT_OK);

    const auto old_app = dartplant::RuntimeIsolateGroup(runtime).app_module;
    const auto old_dart_runtime = dartplant::RuntimeEngine(runtime).module;
    const ModuleSelection app_selection = SelectProfileModule(
        modules, runtime->profile.app_module_name, runtime->profile.app_build_id);
    const ModuleSelection dart_runtime_selection = SelectProfileModule(
        modules, runtime->profile.runtime_module_name, runtime->profile.runtime_build_id,
        dartplant::RuntimeEngine(runtime).anchor);
    RuntimeDiagnostics(runtime).module_candidate_count =
        app_selection.candidate_count + dart_runtime_selection.candidate_count;
    ProjectActiveDiagnostics(runtime);
    if (app_selection.state == ModuleSelectionState::kAmbiguous ||
        dart_runtime_selection.state == ModuleSelectionState::kAmbiguous) {
        if (runtime->generation->load(std::memory_order_acquire) == UINT64_MAX) {
            dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
            SetLastError(
                "runtime generation space is exhausted during ambiguous module transition");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        runtime->generation->fetch_add(1, std::memory_order_acq_rel);
        dartplant::RuntimeProcess(runtime).modules = modules;
        dartplant::RuntimeImageSet staged;
        const DartPlantStatus invalidation_status =
            dartplant::TransitionRuntimeImages(runtime, std::move(staged), modules, true);
        auto& group = dartplant::RuntimeIsolateGroup(runtime);
        auto& engine = dartplant::RuntimeEngine(runtime);
        group.live_vm_context.reset();
        group.capability_bindings = {};
        group.live_vm_null_value = 0;
        group.live_vm_bool_true_value = 0;
        group.live_vm_bool_false_value = 0;
        group.artifact_snapshot_index.reset();
        group.bound_artifact_snapshot_generation = 0;
        group.retired = true;
        if (dart_runtime_selection.state == ModuleSelectionState::kAmbiguous) {
            engine.retired = true;
        }
        dartplant::SetActiveProfileMatched(runtime, false);
        dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
        if (invalidation_status != DARTPLANT_OK) return invalidation_status;
        SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_MODULE_SELECTION,
                              DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                              DARTPLANT_REJECT_MODULE_AMBIGUOUS);
        SetLastError("runtime profile module identity is ambiguous");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const auto& new_app = app_selection.module;
    const auto& new_dart_runtime = dart_runtime_selection.module;
    const bool app_changed = !SameModuleIdentity(old_app, new_app);
    const bool dart_runtime_changed = !SameModuleIdentity(old_dart_runtime, new_dart_runtime);

    std::optional<FlutterSnapshotSource> snapshot;
    std::string snapshot_error;
    if (!app_changed && dartplant::RuntimeIsolateGroup(runtime).snapshot.has_value()) {
        snapshot = dartplant::RuntimeIsolateGroup(runtime).snapshot;
    } else if (new_app.has_value()) {
        snapshot = DiscoverFlutterSnapshot(*new_app, &snapshot_error);
    }
    const bool snapshot_changed =
        !SameSnapshotIdentity(dartplant::RuntimeIsolateGroup(runtime).snapshot, snapshot);
    std::string image_set_error;
    auto image_set = BuildRuntimeImageSet(modules, new_app, snapshot,
                                          runtime->generation->load(std::memory_order_acquire),
                                          &image_set_error);
    if (!image_set.has_value()) {
        dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
        SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_SNAPSHOT_IDENTITY,
                              DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                              DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE);
        SetLastError(image_set_error.empty() ? "runtime image set is malformed" : image_set_error);
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const bool image_set_changed =
        !dartplant::RuntimeIsolateGroup(runtime).image_set.SameIdentity(*image_set);
    const bool incarnation_changed = app_changed || dart_runtime_changed || snapshot_changed;

    auto& process = dartplant::RuntimeProcess(runtime);
    auto& engine = dartplant::RuntimeEngine(runtime);
    auto& group = dartplant::RuntimeIsolateGroup(runtime);
    if (!group.image_set.empty() && !image_set->ReconcileOwnershipFrom(group.image_set)) {
        dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
        SetLastError("runtime image ownership could not be reconciled across refresh");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    uint64_t candidate_engine_epoch = engine.incarnation_epoch;
    const bool allocate_engine_epoch = dart_runtime_changed && new_dart_runtime.has_value();
    if (allocate_engine_epoch) {
        if (!EpochAvailable(process.next_engine_incarnation_epoch)) {
            dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
            SetLastError("Flutter engine incarnation epoch space is exhausted");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        candidate_engine_epoch = process.next_engine_incarnation_epoch;
    }

    uint64_t candidate_group_epoch = group.incarnation_epoch;
    const bool allocate_group_epoch =
        incarnation_changed && new_app.has_value() && snapshot.has_value();
    if (allocate_group_epoch) {
        if (!EpochAvailable(process.next_isolate_group_incarnation_epoch)) {
            dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
            SetLastError("Dart isolate-group incarnation epoch space is exhausted");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        candidate_group_epoch = process.next_isolate_group_incarnation_epoch;
    }

    const uint64_t current_generation = runtime->generation->load(std::memory_order_acquire);
    if (incarnation_changed && current_generation == UINT64_MAX) {
        dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
        SetLastError("runtime generation space is exhausted");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const uint64_t candidate_generation =
        incarnation_changed ? current_generation + 1 : current_generation;
    image_set->BindGeneration(candidate_generation);
    image_set->BindOwnerEpochs(candidate_engine_epoch, candidate_group_epoch);

    // The process module universe is an observation, not a semantic-owner
    // publication. It may advance even if owner transition later fails.
    process.modules = modules;
    if (incarnation_changed) {
        runtime->generation->fetch_add(1, std::memory_order_acq_rel);
    }
    if (incarnation_changed || image_set_changed || group.image_set.empty()) {
        const DartPlantStatus transition_status =
            TransitionRuntimeImages(runtime, std::move(*image_set), modules, incarnation_changed);
        if (transition_status != DARTPLANT_OK) {
            dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
            return transition_status;
        }
    }

    if (allocate_engine_epoch) {
        process.next_engine_incarnation_epoch = candidate_engine_epoch + 1;
    }
    if (allocate_group_epoch) {
        process.next_isolate_group_incarnation_epoch = candidate_group_epoch + 1;
    }
    if (new_dart_runtime.has_value()) {
        engine.module = new_dart_runtime;
        engine.incarnation_epoch = candidate_engine_epoch;
        engine.retired = false;
    } else {
        engine.retired = true;
    }
    if (new_dart_runtime.has_value() && new_app.has_value()) {
        group.app_module = new_app;
        if (snapshot.has_value()) {
            group.incarnation_epoch = candidate_group_epoch;
            group.snapshot = std::move(snapshot);
        } else {
            group.snapshot.reset();
        }
        group.retired = false;
    } else {
        group.retired = true;
    }
    if (incarnation_changed) {
        group.isolate_group_identity = 0;
        group.isolate_generation = 0;
        group.live_vm_context.reset();
        group.capability_bindings = {};
        group.live_vm_null_value = 0;
        group.live_vm_bool_true_value = 0;
        group.live_vm_bool_false_value = 0;
        group.artifact_snapshot_index.reset();
        group.bound_artifact_snapshot_generation = 0;
    } else if (image_set_changed) {
        group.capability_bindings.Clear(vm_abi::kCapabilityDeferredLoadingUnitLayout);
    }

    if (runtime->generation->load(std::memory_order_acquire) != candidate_generation) {
        dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
        SetLastError("runtime generation changed during image transition");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    dartplant::SetActiveProfileMatched(runtime,
                                       new_app.has_value() && new_dart_runtime.has_value());
    if (!runtime->profile_matched) {
        dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_CREATED);
        SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_MODULE_SELECTION,
                              DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                              DARTPLANT_REJECT_MODULE_NOT_FOUND);
        SetLastError("runtime profile modules are not loaded or mismatch");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_SNAPSHOT_IDENTITY,
                          DARTPLANT_RESOLVE_IN_PROGRESS, DARTPLANT_OK);
    if (!dartplant::RuntimeIsolateGroup(runtime).snapshot.has_value()) {
        dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
        SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_SNAPSHOT_IDENTITY,
                              DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                              DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE);
        SetLastError(snapshot_error.empty()
                         ? "loaded app module has no usable Flutter snapshot source"
                         : snapshot_error);
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (incarnation_changed || image_set_changed || runtime->state != DARTPLANT_RUNTIME_READY) {
        dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_IMAGES_READY);
    }
    SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_SNAPSHOT_IDENTITY, DARTPLANT_RESOLVE_RESOLVED,
                          DARTPLANT_OK);
    ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus RefreshRuntimeOwnerTree(DartPlantRuntime* runtime,
                                        const std::vector<ModuleImage>& modules) {
    if (runtime == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    std::lock_guard lock(runtime->mutex);

    auto& process = RuntimeProcess(runtime);
    if (process.engines.empty()) {
        SetLastError("runtime process has no engine owner slots");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    const size_t saved_engine_index = process.active_engine_index;
    std::vector<size_t> saved_group_indices;
    saved_group_indices.reserve(process.engines.size());
    for (const auto& engine : process.engines) {
        saved_group_indices.push_back(engine == nullptr ? 0 : engine->active_isolate_group_index);
    }

    // A loader may replace an engine mapping without delivering a usable
    // pre-unload callback. Detect that physical incarnation change before any
    // ordinary refresh can overwrite the historical engine child in place.
    std::vector<size_t> remapped_engines;
    const size_t observed_engine_count = process.engines.size();
    for (size_t index = 0; index < observed_engine_count; ++index) {
        const auto& engine = process.engines[index];
        if (engine == nullptr || engine->retired || !engine->module.has_value()) continue;
        const ModuleSelection selection =
            SelectProfileModule(modules, runtime->profile.runtime_module_name,
                                runtime->profile.runtime_build_id, engine->anchor);
        if (selection.state == ModuleSelectionState::kUnique && selection.module.has_value() &&
            !SameModuleIdentity(engine->module, selection.module)) {
            remapped_engines.push_back(index);
        }
    }
    for (const size_t index : remapped_engines) {
        process.active_engine_index = index;
        ProjectActiveGeneration(runtime);
        ProjectActiveRuntimeState(runtime);
        const uintptr_t anchor = RuntimeEngine(runtime).anchor;
        const DartPlantStatus retire_status = RetireActiveEngineOwnerLocked(runtime, modules);
        if (retire_status != DARTPLANT_OK) {
            RestoreOwnerProjectionLocked(runtime, saved_engine_index, saved_group_indices);
            return retire_status;
        }
        ActivateEngineOwnerForAnchorLockedInternal(runtime, modules, anchor);
    }

    // A previously retired engine can reappear at the same executable anchor.
    // Give those anchors a chance to materialize a fresh physical owner before
    // walking the currently live owner tree. ActivateEngine... never revives a
    // retired child; it either selects an existing live owner or appends a new
    // one with a distinct incarnation epoch to be assigned by refresh.
    std::vector<uintptr_t> retired_anchors;
    retired_anchors.reserve(process.engines.size());
    for (const auto& engine : process.engines) {
        if (engine != nullptr && engine->retired && engine->anchor != 0) {
            retired_anchors.push_back(engine->anchor);
        }
    }
    for (const uintptr_t anchor : retired_anchors) {
        ActivateEngineOwnerForAnchorLockedInternal(runtime, modules, anchor);
    }

    DartPlantStatus aggregate_status = DARTPLANT_OK;
    std::string first_error;
    size_t refreshed_owner_count = 0;
    const ModuleSelection app_selection = SelectProfileModule(
        modules, runtime->profile.app_module_name, runtime->profile.app_build_id);
    const size_t engine_count = process.engines.size();
    for (size_t engine_index = 0; engine_index < engine_count; ++engine_index) {
        auto& engine = *process.engines[engine_index];
        if (engine.retired) continue;
        process.active_engine_index = engine_index;
        ProjectActiveGeneration(runtime);
        ProjectActiveRuntimeState(runtime);

        // Preserve the old isolate-group owner when the app mapping itself was
        // replaced without a pre-unload notification. A new physical app
        // incarnation receives a fresh group child/epoch under the same live
        // engine rather than mutating the old group in place.
        if (app_selection.state == ModuleSelectionState::kUnique &&
            app_selection.module.has_value()) {
            const size_t observed_group_count = engine.isolate_groups.size();
            for (size_t group_index = 0; group_index < observed_group_count; ++group_index) {
                const auto& group = engine.isolate_groups[group_index];
                if (group == nullptr || group->retired || !group->app_module.has_value() ||
                    SameModuleIdentity(group->app_module, app_selection.module)) {
                    continue;
                }
                engine.active_isolate_group_index = group_index;
                ProjectActiveGeneration(runtime);
                ProjectActiveRuntimeState(runtime);
                const DartPlantStatus retire_status =
                    RetireActiveIsolateGroupOwnerLocked(runtime, modules);
                if (retire_status != DARTPLANT_OK) {
                    RestoreOwnerProjectionLocked(runtime, saved_engine_index, saved_group_indices);
                    return retire_status;
                }
            }
        }
        EnsureFreshIsolateGroupOwnerLocked(runtime);
        const size_t group_count = engine.isolate_groups.size();
        const size_t original_group_index = engine.active_isolate_group_index;
        for (size_t group_index = 0; group_index < group_count; ++group_index) {
            const auto& group = engine.isolate_groups[group_index];
            if (group == nullptr || group->retired) continue;
            process.active_engine_index = engine_index;
            engine.active_isolate_group_index = group_index;
            ProjectActiveGeneration(runtime);
            ProjectActiveRuntimeState(runtime);

            ++refreshed_owner_count;
            const DartPlantStatus status = RefreshRuntimeModules(runtime, modules);
            if (status != DARTPLANT_OK && aggregate_status == DARTPLANT_OK) {
                aggregate_status = status;
                first_error = LastError();
            }
        }
        engine.active_isolate_group_index =
            std::min(original_group_index,
                     engine.isolate_groups.empty() ? size_t{0} : engine.isolate_groups.size() - 1);
    }
    RestoreOwnerProjectionLocked(runtime, saved_engine_index, saved_group_indices);

    if (refreshed_owner_count == 0) {
        aggregate_status = DARTPLANT_RUNTIME_NOT_READY;
        first_error = "runtime owner tree has no active engine/isolate-group owner to refresh";
    }
    if (aggregate_status != DARTPLANT_OK) {
        SetLastError(first_error.empty() ? "one or more runtime owners failed to refresh"
                                         : first_error);
    } else {
        ClearLastError();
    }
    return aggregate_status;
}

void StartRuntimeModuleRefreshWorker(RuntimeModuleRefreshReporter reporter) {
    auto& worker = RuntimeRefreshWorker();
    if (reporter != nullptr) worker.reporter.store(reporter, std::memory_order_release);
    bool expected = false;
    if (!worker.started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    std::thread([] {
        auto& state = RuntimeRefreshWorker();
        uint64_t processed_epoch = 0;
        for (;;) {
            state.requested_epoch.wait(processed_epoch, std::memory_order_acquire);
            const uint64_t epoch = state.requested_epoch.load(std::memory_order_acquire);

            auto operations = PinRuntimeRefreshes();
            auto modules = EnumerateModules();
            ReplaceModules(modules);

            DartPlantStatus status = DARTPLANT_OK;
            std::string error;
            for (const auto& operation : operations) {
                const DartPlantStatus refresh_status =
                    RefreshRuntimeOwnerTree(operation.registration->runtime, modules);
                if (refresh_status != DARTPLANT_OK && status == DARTPLANT_OK) {
                    status = refresh_status;
                    error = LastError();
                }
            }

            RuntimeModuleRefreshReporter reporter = nullptr;
            {
                std::lock_guard lock(state.mutex);
                state.completed_epoch = epoch;
                state.results.push_back(
                    {.first_epoch = processed_epoch + 1, .last_epoch = epoch, .status = status});
                if (state.results.size() > 64) state.results.pop_front();
                reporter = state.reporter.load(std::memory_order_acquire);
            }
            processed_epoch = epoch;
            state.completed.notify_all();
            operations.clear();
            if (status != DARTPLANT_OK && reporter != nullptr) {
                reporter(status, error.empty() ? "runtime module refresh failed" : error.c_str());
            }
        }
    }).detach();
}

uint64_t ScheduleRuntimeModuleRefresh() {
    auto& worker = RuntimeRefreshWorker();
    if (!worker.started.load(std::memory_order_acquire)) return 0;
    const uint64_t epoch = worker.requested_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    worker.requested_epoch.notify_one();
    return epoch;
}

DartPlantStatus WaitForRuntimeModuleRefresh(uint64_t epoch) {
    if (epoch == 0) return DARTPLANT_OK;
    auto& worker = RuntimeRefreshWorker();
    std::unique_lock lock(worker.mutex);
    worker.completed.wait(lock, [&worker, epoch] { return worker.completed_epoch >= epoch; });
    if (worker.results.empty() || epoch < worker.results.front().first_epoch) {
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const auto found = std::lower_bound(
        worker.results.begin(), worker.results.end(), epoch,
        [](const auto& result, uint64_t requested) { return result.last_epoch < requested; });
    return found == worker.results.end() || epoch < found->first_epoch ? DARTPLANT_RUNTIME_NOT_READY
                                                                       : found->status;
}

DartPlantStatus BuildLiveIndexForContext(DartPlantRuntime* runtime,
                                         const DartPlantLiveVmContext& context,
                                         const FlutterSnapshotSource& snapshot,
                                         uint64_t validated_null_value,
                                         DartPlantVmAdapter* observation_adapter,
                                         void* observation_lease) {
    if (runtime == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    if (!VmAdapterOwnsLiveHeapObservation(observation_adapter, context.thread, observation_lease)) {
        SetLastError(
            "live Function index construction requires the current thread's exact moving-GC "
            "observation receipt");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    std::lock_guard runtime_lock(runtime->mutex);
    if (validated_null_value == 0 ||
        !dartplant_vm_abi_is_tagged_heap_object(validated_null_value)) {
        SetLastError("live VM context has no validated Dart NULL_REG value");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    FillSnapshotInfo(snapshot, &snapshot_info);
    VmRuntimeFacts facts{};
    facts.snapshot_hash = snapshot_info.snapshot_hash == nullptr ? "" : snapshot_info.snapshot_hash;
    facts.snapshot_features =
        snapshot_info.snapshot_features == nullptr ? "" : snapshot_info.snapshot_features;
    facts.compressed_pointers = snapshot_info.compressed_pointers != 0;
    vm_abi::ResolverInput resolver_input{};
    resolver_input.facts = facts;
    resolver_input.thread = context.thread;
    resolver_input.current_isolate = context.isolate;
    // The captured NULL_REG value proved register semantics at capture time.
    // Do not require its raw heap address to remain identical here: canonical
    // tagged objects are GC-managed evidence. The fresh RootProof below is the
    // authority for the current Thread::object_null() value.
    resolver_input.canonical_null = 0;
    resolver_input.registers = {.available = false};
    resolver_input.modules = &dartplant::RuntimeProcess(runtime).modules;
    const vm_abi::ResolverResult resolver = vm_abi::ResolveVerifiedBinding(resolver_input);
    if (!resolver.passed || resolver.binding.core.representative == nullptr) {
        SetLastError(resolver.distinct_abis > 1
                         ? "live VM runtime-root capability is ambiguous across candidate profiles"
                         : "live VM runtime-root capability rejected every candidate profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }

    // ResolveVerifiedBinding() has just re-read Thread -> IsolateGroup roots
    // from the live VM. Those freshly proven values, not the register-sample
    // snapshot, are authoritative for every heap walk below. This matters when
    // a compacting GC runs between capture and index construction: Dart updates
    // Thread::global_object_pool_ and the group root graph, while a copied raw
    // ObjectPoolPtr/PP value is intentionally outside the GC root set.
    const vm_abi::RootProof& fresh_roots = resolver.binding.core.core_probe.roots;
    if (!fresh_roots.passed || fresh_roots.isolate_group == 0 ||
        fresh_roots.isolate_group != context.isolate_group || fresh_roots.class_table == 0 ||
        fresh_roots.object_store == 0 || fresh_roots.heap_base == 0 ||
        fresh_roots.global_object_pool == 0 || fresh_roots.thread_null == 0) {
        SetLastError("live VM root graph changed between capture and runtime proof");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    DartPlantLiveVmContext live_context = context;
    live_context.isolate = fresh_roots.isolate;
    live_context.isolate_group = fresh_roots.isolate_group;
    live_context.class_table = fresh_roots.class_table;
    live_context.cached_class_table_table = fresh_roots.cached_class_table_table;
    live_context.object_store = fresh_roots.object_store;
    live_context.heap_base = fresh_roots.heap_base;
    live_context.global_object_pool = fresh_roots.global_object_pool;
    live_context.object_pool_length = fresh_roots.object_pool_length;
    if (fresh_roots.global_object_pool <
        resolver.binding.core.representative->raw_object.heap_object_tag) {
        SetLastError("fresh live VM ObjectPool root has an invalid tagged address");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    live_context.pp = fresh_roots.global_object_pool -
                      resolver.binding.core.representative->raw_object.heap_object_tag;
    const uint64_t live_null_value = fresh_roots.thread_null;

    const DartPlantStatus owner_status =
        ActivateRuntimeIsolateGroupOwnerForContextLocked(runtime, live_context, snapshot);
    if (owner_status != DARTPLANT_OK) return owner_status;

    vm_abi::CapabilityBindingSet capability_bindings = resolver.binding.capability_bindings;
    const vm_abi::CapabilityBinding* live_index_binding =
        capability_bindings.Find(vm_abi::kCapabilityLiveFunctionIndexLayout);
    if (live_index_binding == nullptr || !live_index_binding->bound()) {
        SetLastError("live Function index capability is ambiguous across VM ABI candidates");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const RuntimeProfileRecord* live_index_profile = live_index_binding->representative;
    const vm_abi::CapabilityBinding* function_type_binding =
        capability_bindings.Find(vm_abi::kCapabilityFunctionTypeLayout);
    if (function_type_binding == nullptr || !function_type_binding->bound() ||
        function_type_binding->representative == nullptr) {
        SetLastError("FunctionType capability is ambiguous across VM ABI candidates");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const RuntimeProfileRecord* function_type_profile = function_type_binding->representative;

    uint64_t bool_true_value = 0;
    uint64_t bool_false_value = 0;
    {
        std::vector<bool> compatible;
        std::vector<uint64_t> candidate_true;
        std::vector<uint64_t> candidate_false;
        compatible.reserve(resolver.binding.core.candidates.profiles.size());
        candidate_true.reserve(resolver.binding.core.candidates.profiles.size());
        candidate_false.reserve(resolver.binding.core.candidates.profiles.size());
        for (const RuntimeProfileRecord* candidate : resolver.binding.core.candidates.profiles) {
            uint64_t candidate_true_value = 0;
            uint64_t candidate_false_value = 0;
            const bool matches =
                candidate != nullptr && ProbeLiveVmCanonicalBoolRootsForCandidate(
                                            live_context, *candidate, &candidate_true_value,
                                            &candidate_false_value) == DARTPLANT_OK;
            compatible.push_back(matches);
            candidate_true.push_back(candidate_true_value);
            candidate_false.push_back(candidate_false_value);
        }
        const auto bool_selection = vm_abi::SelectCapabilityAbiSet(
            resolver.binding.core.candidates, vm_abi::kCapabilityCanonicalNull, compatible);
        if (!bool_selection.passed() || bool_selection.representative == nullptr ||
            !capability_bindings.Bind(vm_abi::kCapabilityCanonicalNull, bool_selection)) {
            SetLastError(bool_selection.ambiguous()
                             ? "canonical Bool capability is ambiguous across VM ABI candidates"
                             : "canonical Bool capability rejected every VM ABI candidate");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        const auto selected_it = std::find(resolver.binding.core.candidates.profiles.begin(),
                                           resolver.binding.core.candidates.profiles.end(),
                                           bool_selection.representative);
        if (selected_it == resolver.binding.core.candidates.profiles.end()) {
            SetLastError("canonical Bool capability selected an unknown VM ABI row");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        const size_t selected_index =
            static_cast<size_t>(selected_it - resolver.binding.core.candidates.profiles.begin());
        if (!compatible[selected_index]) {
            SetLastError("canonical Bool capability lost its runtime proof");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        bool_true_value = candidate_true[selected_index];
        bool_false_value = candidate_false[selected_index];
    }

    std::optional<uint32_t> root_program_hash;
    const RuntimeProfileRecord* deferred_profile = nullptr;
    if (dartplant::RuntimeIsolateGroup(runtime).image_set.size() > 1) {
        std::vector<bool> compatible;
        std::vector<uint32_t> candidate_program_hashes;
        compatible.reserve(resolver.binding.core.candidates.profiles.size());
        candidate_program_hashes.reserve(resolver.binding.core.candidates.profiles.size());
        for (const RuntimeProfileRecord* candidate : resolver.binding.core.candidates.profiles) {
            uint32_t program_hash = 0;
            bool matches = candidate != nullptr &&
                           ProbeLiveVmRootProgramHashForCandidate(live_context, *candidate,
                                                                  &program_hash) == DARTPLANT_OK;
            if (matches) {
                for (const auto& image :
                     dartplant::RuntimeIsolateGroup(runtime).image_set.images()) {
                    if (image.kind != RuntimeImageKind::kDeferred) continue;
                    if (!image.snapshot.deferred_program_hash.has_value() ||
                        *image.snapshot.deferred_program_hash != program_hash) {
                        matches = false;
                        break;
                    }
                }
            }
            compatible.push_back(matches);
            candidate_program_hashes.push_back(program_hash);
        }
        const auto deferred_selection = vm_abi::SelectCapabilityAbiSet(
            resolver.binding.core.candidates, vm_abi::kCapabilityDeferredLoadingUnitLayout,
            compatible);
        if (!deferred_selection.passed() || deferred_selection.representative == nullptr) {
            SetLastError(
                deferred_selection.ambiguous()
                    ? "deferred LoadingUnit capability is ambiguous across VM ABI candidates"
                    : "deferred LoadingUnit capability rejected every VM ABI candidate");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        deferred_profile = deferred_selection.representative;
        if (!capability_bindings.Bind(vm_abi::kCapabilityDeferredLoadingUnitLayout,
                                      deferred_selection)) {
            SetLastError("deferred LoadingUnit capability could not publish its binding");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        const auto selected_it =
            std::find(resolver.binding.core.candidates.profiles.begin(),
                      resolver.binding.core.candidates.profiles.end(), deferred_profile);
        if (selected_it == resolver.binding.core.candidates.profiles.end()) {
            SetLastError("deferred LoadingUnit capability selected an unknown VM ABI row");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        const size_t selected_index =
            static_cast<size_t>(selected_it - resolver.binding.core.candidates.profiles.begin());
        if (!compatible[selected_index]) {
            SetLastError("deferred LoadingUnit capability lost its runtime proof");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        root_program_hash = candidate_program_hashes[selected_index];
    }

    auto& process_state = dartplant::RuntimeProcess(runtime);
    auto& engine_state = dartplant::RuntimeEngine(runtime);
    auto& group_state = dartplant::RuntimeIsolateGroup(runtime);
    const bool semantic_context_changed =
        group_state.live_vm_context.has_value() &&
        !SameRuntimeIsolateGroupSemanticContext(*group_state.live_vm_context, live_context);
    if (semantic_context_changed) {
        const uint64_t old_generation = runtime->generation->load(std::memory_order_acquire);
        if (old_generation == UINT64_MAX) {
            dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
            SetLastError("isolate-group semantic generation space is exhausted");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        const uint64_t next_generation = old_generation + 1;
        RuntimeImageSet staged_images = group_state.image_set;
        staged_images.BindGeneration(next_generation);
        staged_images.BindOwnerEpochs(engine_state.incarnation_epoch,
                                      group_state.incarnation_epoch);

        // The isolate group remains the same owner; only its semantic
        // generation advances. Close the previous generation before any new
        // Function index is published, but do not consume a new group epoch.
        runtime->generation->fetch_add(1, std::memory_order_acq_rel);
        const DartPlantStatus transition_status =
            TransitionRuntimeImages(runtime, std::move(staged_images), process_state.modules, true);
        if (transition_status != DARTPLANT_OK) {
            dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
            return transition_status;
        }
        group_state.isolate_generation = group_state.isolate_generation == UINT64_MAX
                                             ? UINT64_MAX
                                             : group_state.isolate_generation + 1;
        group_state.live_vm_context.reset();
        group_state.capability_bindings = {};
        group_state.live_vm_null_value = 0;
        group_state.live_vm_bool_true_value = 0;
        group_state.live_vm_bool_false_value = 0;
    }

    DartPlantLiveVmFunctionIndexInfo index_info{};
    index_info.struct_size = sizeof(index_info);
    std::string error;
    std::optional<SnapshotIndex> index;
    if (!dartplant::RuntimeIsolateGroup(runtime).image_set.empty()) {
        std::vector<LiveVmInstructionImage> instruction_images;
        instruction_images.reserve(dartplant::RuntimeIsolateGroup(runtime).image_set.size());
        for (const auto& image : dartplant::RuntimeIsolateGroup(runtime).image_set.images()) {
            LiveVmInstructionImage descriptor{};
            descriptor.runtime_image_id = image.id;
            descriptor.runtime_image_incarnation_epoch = image.incarnation_epoch;
            descriptor.engine_incarnation_epoch = image.engine_incarnation_epoch;
            descriptor.isolate_group_incarnation_epoch = image.isolate_group_incarnation_epoch;
            descriptor.runtime_generation = image.runtime_generation;
            descriptor.loading_unit_id = image.loading_unit_id;
            descriptor.snapshot.struct_size = sizeof(descriptor.snapshot);
            FillSnapshotInfo(image.snapshot, &descriptor.snapshot);
            instruction_images.push_back(descriptor);
        }
        index = BuildLiveSnapshotIndexForImages(live_context, instruction_images,
                                                *live_index_profile, *function_type_profile,
                                                deferred_profile, &index_info, &error);
    } else {
        // Synthetic/legacy callers that seed runtime fields directly retain the
        // single-image contract until they opt into RuntimeImageSet.
        index = BuildLiveSnapshotIndex(live_context, snapshot_info, *live_index_profile,
                                       *function_type_profile, &index_info, &error);
    }
    if (!index.has_value()) {
        SetLastError(error.empty() ? "failed to build live Function index" : error);
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::optional<RuntimeImageSet> semantically_bound_images;
    if (!dartplant::RuntimeIsolateGroup(runtime).image_set.empty()) {
        RuntimeImageSet rebound;
        if (!BindLiveSnapshotImageSemantics(
                *index, dartplant::RuntimeIsolateGroup(runtime).image_set, &rebound, &error)) {
            SetLastError(error.empty() ? "live Function image semantics are inconsistent" : error);
            return DARTPLANT_PROFILE_MISMATCH;
        }
        if (root_program_hash.has_value() && !rebound.BindDeferredProgramHash(*root_program_hash)) {
            char mismatch[192] = {};
            for (const auto& image : rebound.images()) {
                if (image.kind != RuntimeImageKind::kDeferred) continue;
                if (!image.snapshot.deferred_program_hash.has_value() ||
                    *image.snapshot.deferred_program_hash == *root_program_hash) {
                    continue;
                }
                std::snprintf(mismatch, sizeof(mismatch),
                              "deferred runtime image program hash disagrees with the live isolate "
                              "group (unit=%u artifact=0x%08x live_root=0x%08x)",
                              image.loading_unit_id, *image.snapshot.deferred_program_hash,
                              *root_program_hash);
                break;
            }
            SetLastError(
                mismatch[0] == '\0'
                    ? "deferred runtime image program hash disagrees with the live isolate "
                      "group"
                    : mismatch);
            return DARTPLANT_PROFILE_MISMATCH;
        }
        semantically_bound_images = std::move(rebound);
    }
    if (semantically_bound_images.has_value()) {
        semantically_bound_images->BindGeneration(
            runtime->generation->load(std::memory_order_acquire));
        semantically_bound_images->BindOwnerEpochs(
            dartplant::RuntimeEngine(runtime).incarnation_epoch,
            dartplant::RuntimeIsolateGroup(runtime).incarnation_epoch);
        semantically_bound_images->ActivateAll();
        dartplant::RuntimeIsolateGroup(runtime).image_set = std::move(*semantically_bound_images);
    }
    capability_bindings.StampOwner(RuntimeCapabilityOwner(runtime));
    if (!capability_bindings.owner.valid()) {
        SetLastError("VM capability bindings have no current runtime owner epoch");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    dartplant::RuntimeIsolateGroup(runtime).live_vm_context = live_context;
    dartplant::RuntimeIsolateGroup(runtime).isolate_group_identity = live_context.isolate_group;
    if (dartplant::RuntimeIsolateGroup(runtime).isolate_generation == 0) {
        dartplant::RuntimeIsolateGroup(runtime).isolate_generation = 1;
    }
    dartplant::RuntimeIsolateGroup(runtime).capability_bindings = std::move(capability_bindings);
    dartplant::RuntimeIsolateGroup(runtime).live_vm_null_value = live_null_value;
    dartplant::RuntimeIsolateGroup(runtime).live_vm_bool_true_value = bool_true_value;
    dartplant::RuntimeIsolateGroup(runtime).live_vm_bool_false_value = bool_false_value;
    dartplant::RuntimeIsolateGroup(runtime).live_snapshot_index = std::move(index);
    dartplant::RuntimeIsolateGroup(runtime).live_function_index_info = index_info;
    dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_READY);
    ClearLastError();
    return DARTPLANT_OK;
}

}  // namespace dartplant

extern "C" {

DartPlantStatus dartplant_runtime_create(const DartPlantRuntimeProfile* profile,
                                         DartPlantRuntime** out_runtime) {
    if (profile == nullptr || out_runtime == nullptr ||
        profile->struct_size < sizeof(DartPlantRuntimeProfile) || profile->profile_version != 1 ||
        profile->runtime_kind != DARTPLANT_RUNTIME_FLUTTER_AOT ||
        profile->architecture != DARTPLANT_ARCH_ARM64 || profile->pointer_size != 8 ||
        profile->app_module_name == nullptr || profile->runtime_module_name == nullptr) {
        dartplant::SetLastError("runtime profile is invalid or unsupported");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    if (profile->argument_count > 8) {
        dartplant::SetLastError("runtime profile has too many GP arguments");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    auto* runtime = new DartPlantRuntime;
    runtime->profile.Assign(*profile);
    dartplant::RegisterRuntime(runtime);
    dartplant::RefreshRuntimeModules(runtime, dartplant::EnumerateModules());
    *out_runtime = runtime;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

void dartplant_runtime_destroy(DartPlantRuntime* runtime) {
    if (runtime == nullptr) return;
    if (dartplant::CloseRuntime(runtime) == nullptr) return;
    const auto modules = dartplant::EnumerateModules();
    struct OwnerDrain {
        std::shared_ptr<std::atomic_uint64_t> generation;
        bool app_mapping_is_current = false;
    };
    std::vector<OwnerDrain> owners;
    {
        std::lock_guard lock(runtime->mutex);
        for (const auto& engine : runtime->process.engines) {
            if (engine == nullptr) continue;
            for (const auto& group : engine->isolate_groups) {
                if (group == nullptr || group->generation == nullptr) continue;
                owners.push_back({
                    .generation = group->generation,
                    .app_mapping_is_current =
                        dartplant::ContainsModuleIdentity(modules, group->app_module),
                });
            }
        }
    }
    for (const auto& owner : owners) {
        owner.generation->fetch_add(1, std::memory_order_acq_rel);
        if (owner.app_mapping_is_current) {
            dartplant::InvalidateRuntimeHooks(owner.generation);
        } else {
            dartplant::RetireRuntimeHooks(owner.generation);
        }
    }
    delete runtime;
}

DartPlantStatus dartplant_runtime_refresh_modules(DartPlantRuntime* runtime) {
    if (runtime == nullptr) {
        dartplant::SetLastError("runtime is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    auto modules = dartplant::EnumerateModules();
    return dartplant::RefreshRuntimeOwnerTree(runtime, modules);
}

DartPlantStatus dartplant_runtime_bind_engine_anchor(DartPlantRuntime* runtime,
                                                     const void* engine_address) {
    if (runtime == nullptr) {
        dartplant::SetLastError("runtime is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) return DARTPLANT_RUNTIME_NOT_READY;
    const auto modules = dartplant::EnumerateModules();
    {
        std::lock_guard lock(runtime->mutex);
        dartplant::ActivateRuntimeEngineOwnerForAnchorLocked(
            runtime, modules, reinterpret_cast<uintptr_t>(engine_address));
    }
    return dartplant::RefreshRuntimeOwnerTree(runtime, modules);
}

DartPlantStatus dartplant_runtime_on_module_loaded(DartPlantRuntime* runtime, const char*, void*) {
    return dartplant_runtime_refresh_modules(runtime);
}

DartPlantStatus dartplant_runtime_on_module_unloading(DartPlantRuntime* runtime,
                                                      const char* module_name, void*) {
    if (runtime == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) return DARTPLANT_RUNTIME_NOT_READY;
    std::lock_guard lock(runtime->mutex);
    auto& process = dartplant::RuntimeProcess(runtime);
    const size_t saved_engine_index = process.active_engine_index;
    std::vector<size_t> saved_group_indices;
    saved_group_indices.reserve(process.engines.size());
    for (const auto& engine : process.engines) {
        saved_group_indices.push_back(engine == nullptr ? 0 : engine->active_isolate_group_index);
    }
    const bool unknown_event = module_name == nullptr || module_name[0] == '\0';
    DartPlantStatus aggregate_status = DARTPLANT_OK;

    const auto remember_failure = [&aggregate_status](DartPlantStatus status) {
        if (aggregate_status == DARTPLANT_OK && status != DARTPLANT_OK) {
            aggregate_status = status;
        }
    };

    for (size_t engine_index = 0; engine_index < process.engines.size(); ++engine_index) {
        auto& engine_owner = *process.engines[engine_index];
        const bool engine_matches =
            unknown_event || (engine_owner.module.has_value() &&
                              dartplant::ModuleMatchesEvent(*engine_owner.module, module_name));
        bool engine_retire_succeeded = engine_matches;
        for (size_t group_index = 0; group_index < engine_owner.isolate_groups.size();
             ++group_index) {
            process.active_engine_index = engine_index;
            engine_owner.active_isolate_group_index = group_index;
            dartplant::ProjectActiveGeneration(runtime);
            dartplant::ProjectActiveRuntimeState(runtime);
            auto& group = dartplant::RuntimeIsolateGroup(runtime);

            const auto deferred =
                std::find_if(group.image_set.images().begin(), group.image_set.images().end(),
                             [module_name](const dartplant::RuntimeImage& image) {
                                 return image.kind == dartplant::RuntimeImageKind::kDeferred &&
                                        dartplant::ModuleMatchesEvent(image.module, module_name);
                             });
            if (deferred != group.image_set.images().end()) {
                dartplant::RuntimeImageSet staged = group.image_set;
                if (!staged.RemoveById(deferred->id)) {
                    dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
                    remember_failure(DARTPLANT_RUNTIME_NOT_READY);
                    continue;
                }
                const DartPlantStatus status = dartplant::TransitionRuntimeImages(
                    runtime, std::move(staged), process.modules, false);
                if (status == DARTPLANT_OK) {
                    group.capability_bindings.Clear(
                        dartplant::vm_abi::kCapabilityDeferredLoadingUnitLayout);
                }
                dartplant::SetActiveRuntimeState(runtime, status == DARTPLANT_OK
                                                              ? DARTPLANT_RUNTIME_IMAGES_READY
                                                              : DARTPLANT_RUNTIME_FAILED);
                remember_failure(status);
                continue;
            }

            const bool app_matches =
                unknown_event || (group.app_module.has_value() &&
                                  dartplant::ModuleMatchesEvent(*group.app_module, module_name));
            if (!app_matches && !engine_matches) continue;
            if (group.generation->load(std::memory_order_acquire) == UINT64_MAX) {
                dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
                if (engine_matches) engine_retire_succeeded = false;
                remember_failure(DARTPLANT_RUNTIME_NOT_READY);
                continue;
            }
            group.generation->fetch_add(1, std::memory_order_acq_rel);
            dartplant::RuntimeImageSet staged;
            const DartPlantStatus status = dartplant::TransitionRuntimeImages(
                runtime, std::move(staged), process.modules, true);
            remember_failure(status);
            if (status != DARTPLANT_OK) {
                if (engine_matches) engine_retire_succeeded = false;
                dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_FAILED);
                continue;
            }

            group.live_vm_context.reset();
            group.capability_bindings = {};
            group.live_snapshot_index.reset();
            group.artifact_snapshot_index.reset();
            group.bound_artifact_snapshot_generation = 0;
            group.live_vm_null_value = 0;
            group.live_vm_bool_true_value = 0;
            group.live_vm_bool_false_value = 0;
            if (app_matches || engine_matches) {
                group.retired = true;
            }
            dartplant::SetActiveProfileMatched(runtime, false);
            dartplant::SetActiveRuntimeState(runtime, DARTPLANT_RUNTIME_CREATED);
        }
        if (engine_matches && engine_retire_succeeded) {
            engine_owner.retired = true;
        }
    }

    dartplant::RestoreOwnerProjectionLocked(runtime, saved_engine_index, saved_group_indices);
    if (aggregate_status != DARTPLANT_OK && dartplant::LastError()[0] == '\0') {
        dartplant::SetLastError("one or more runtime owners failed to drain during pre-unload");
    }
    return aggregate_status;
}

DartPlantStatus dartplant_runtime_get_info(const DartPlantRuntime* runtime,
                                           DartPlantRuntimeInfo* out_info) {
    if (runtime == nullptr || out_info == nullptr ||
        out_info->struct_size < sizeof(DartPlantRuntimeInfo)) {
        dartplant::SetLastError("runtime info arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    out_info->state = runtime->state;
    out_info->loaded_module_count =
        static_cast<uint32_t>(dartplant::RuntimeProcess(runtime).modules.size());
    out_info->app_module_loaded =
        dartplant::RuntimeIsolateGroup(runtime).app_module.has_value() ? 1 : 0;
    out_info->runtime_module_loaded = dartplant::RuntimeEngine(runtime).module.has_value() ? 1 : 0;
    out_info->live_function_index_ready =
        dartplant::RuntimeIsolateGroup(runtime).live_snapshot_index.has_value() ? 1 : 0;
    out_info->profile_matched = runtime->profile_matched ? 1 : 0;
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_get_image_count(const DartPlantRuntime* runtime,
                                                  uint32_t* out_count) {
    if (runtime == nullptr || out_count == nullptr) {
        dartplant::SetLastError("runtime image count arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) return DARTPLANT_RUNTIME_NOT_READY;
    std::lock_guard lock(runtime->mutex);
    if (dartplant::RuntimeIsolateGroup(runtime).image_set.size() > UINT32_MAX) {
        dartplant::SetLastError("runtime image count exceeds public ABI range");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    *out_count = static_cast<uint32_t>(dartplant::RuntimeIsolateGroup(runtime).image_set.size());
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_get_image_info(const DartPlantRuntime* runtime, uint32_t index,
                                                 DartPlantRuntimeImageInfo* out_info) {
    constexpr size_t kRuntimeImageInfoV1Size =
        offsetof(DartPlantRuntimeImageInfo, incarnation_epoch);
    if (runtime == nullptr || out_info == nullptr ||
        out_info->struct_size < kRuntimeImageInfoV1Size) {
        dartplant::SetLastError("runtime image info arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) return DARTPLANT_RUNTIME_NOT_READY;
    std::lock_guard lock(runtime->mutex);
    if (index >= dartplant::RuntimeIsolateGroup(runtime).image_set.size()) {
        dartplant::SetLastError("runtime image index is out of range");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const auto& image = dartplant::RuntimeIsolateGroup(runtime).image_set.images()[index];
    DartPlantRuntimeImageInfo source{};
    source.struct_size = sizeof(source);
    source.kind = image.kind == dartplant::RuntimeImageKind::kRoot
                      ? DARTPLANT_RUNTIME_IMAGE_ROOT
                      : DARTPLANT_RUNTIME_IMAGE_DEFERRED;
    source.image_id = image.id;
    source.runtime_generation = image.runtime_generation;
    source.loading_unit_id = image.loading_unit_id;
    source.module_name = image.module.name.c_str();
    source.module_path = image.module.path.c_str();
    source.module_build_id = image.module.build_id.c_str();
    source.snapshot_hash = image.snapshot.snapshot_hash.c_str();
    source.snapshot_features = image.snapshot.snapshot_features.c_str();
    source.profile_name = image.snapshot.profile_name.c_str();
    source.isolate_instructions_va = image.snapshot.isolate_instructions_va;
    source.isolate_instructions_size = image.snapshot.isolate_instructions_size;
    source.isolate_instructions_runtime = image.snapshot.isolate_instructions_runtime;
    source.live_entry_count = image.live_entry_count;
    source.live_semantic_bound = image.live_entry_count != 0 ? 1 : 0;
    source.has_deferred_program_hash = image.snapshot.deferred_program_hash.has_value() ? 1 : 0;
    source.deferred_program_hash_vm_bound = image.deferred_program_hash_vm_bound ? 1 : 0;
    source.deferred_program_hash = image.snapshot.deferred_program_hash.value_or(0);
    source.incarnation_epoch = image.incarnation_epoch;
    source.engine_incarnation_epoch = image.engine_incarnation_epoch;
    source.isolate_group_incarnation_epoch = image.isolate_group_incarnation_epoch;
    source.lifecycle_state = static_cast<uint32_t>(image.lifecycle);

    const size_t caller_size = out_info->struct_size;
    const size_t written_size = std::min(caller_size, sizeof(source));
    std::memcpy(out_info, &source, written_size);
    out_info->struct_size = static_cast<uint32_t>(written_size);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_get_resolution_diagnostics(
    const DartPlantRuntime* runtime, DartPlantResolutionDiagnostics* out_diagnostics) {
    if (runtime == nullptr || out_diagnostics == nullptr ||
        out_diagnostics->struct_size < sizeof(DartPlantResolutionDiagnostics)) {
        dartplant::SetLastError("runtime diagnostics arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    *out_diagnostics = dartplant::RuntimeDiagnostics(runtime);
    out_diagnostics->struct_size = sizeof(*out_diagnostics);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_get_flutter_snapshot(const DartPlantRuntime* runtime,
                                                       DartPlantFlutterSnapshotInfo* out_info) {
    if (runtime == nullptr || out_info == nullptr || out_info->struct_size < sizeof(*out_info)) {
        dartplant::SetLastError("runtime snapshot info arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    if (!dartplant::RuntimeIsolateGroup(runtime).snapshot.has_value()) {
        dartplant::SetLastError("Flutter snapshot source is not available");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const auto& snapshot = *dartplant::RuntimeIsolateGroup(runtime).snapshot;
    dartplant::FillSnapshotInfo(snapshot, out_info);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_capture_live_vm(DartPlantRuntime* runtime,
                                                  const DartPlantInvocation* invocation) {
    if (runtime == nullptr || invocation == nullptr) {
        dartplant::SetLastError("runtime live VM capture arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                     DARTPLANT_RESOLVE_IN_PROGRESS, DARTPLANT_OK);
    if (!dartplant::RuntimeIsolateGroup(runtime).snapshot.has_value()) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                                         DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE);
        dartplant::SetLastError("Flutter snapshot source is not available for live VM capture");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (invocation->requested_method == nullptr ||
        invocation->requested_method->function == nullptr ||
        !dartplant::IsCurrentRuntimeMethod(runtime, invocation->requested_method) ||
        invocation->hook == nullptr ||
        invocation->hook->runtime_generation != runtime->generation ||
        invocation->hook->expected_runtime_generation !=
            runtime->generation->load(std::memory_order_acquire) ||
        invocation->context == nullptr) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                                         DARTPLANT_REJECT_STALE_GENERATION);
        dartplant::SetLastError(
            "live VM capture invocation belongs to a stale or different runtime generation");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    const dartplant::RuntimeProfileRecord* capture_profile = dartplant::FindRuntimeProfileByVersion(
        invocation->requested_method->function->runtime_profile_version);
    if (capture_profile == nullptr || capture_profile->live_vm.thr_register >= 31 ||
        invocation->vm_adapter == nullptr) {
        dartplant::SetLastError(
            "live VM capture has no exact VM adapter/profile for moving-GC observation");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    const uint64_t observation_thread =
        invocation->context->x[capture_profile->live_vm.thr_register];
    void* observation_lease = nullptr;
    DartPlantStatus status = dartplant::VmAdapterBeginLiveHeapObservation(
        invocation->vm_adapter, observation_thread, &observation_lease);
    if (status != DARTPLANT_OK) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, status,
                                         DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE);
        return status;
    }

    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    dartplant::FillSnapshotInfo(*dartplant::RuntimeIsolateGroup(runtime).snapshot, &snapshot_info);
    DartPlantLiveVmProbeInfo probe{};
    probe.struct_size = sizeof(probe);
    status = dartplant_live_vm_probe_invocation(invocation, &snapshot_info, &probe);
    if (status != DARTPLANT_OK) {
        const DartPlantStatus release_status =
            dartplant::VmAdapterEndLiveHeapObservation(invocation->vm_adapter, observation_lease);
        if (release_status != DARTPLANT_OK) return release_status;
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, status,
                                         DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE);
        return status;
    }

    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    status = dartplant_live_vm_context_from_probe(&probe, &context);
    if (status != DARTPLANT_OK) {
        const DartPlantStatus release_status =
            dartplant::VmAdapterEndLiveHeapObservation(invocation->vm_adapter, observation_lease);
        if (release_status != DARTPLANT_OK) return release_status;
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, status,
                                         DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE);
        return status;
    }
    const dartplant::RuntimeProfileRecord* probe_profile =
        dartplant::FindRuntimeProfileByVersion(probe.profile_version);
    if (probe_profile == nullptr || probe_profile->live_vm.null_register >= 31) {
        const DartPlantStatus release_status =
            dartplant::VmAdapterEndLiveHeapObservation(invocation->vm_adapter, observation_lease);
        if (release_status != DARTPLANT_OK) return release_status;
        dartplant::SetLastError("live VM probe selected an invalid candidate profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    status = dartplant::BuildLiveIndexForContext(
        runtime, context, *dartplant::RuntimeIsolateGroup(runtime).snapshot,
        invocation->context->x[probe_profile->live_vm.null_register], invocation->vm_adapter,
        observation_lease);
    const DartPlantStatus release_status =
        dartplant::VmAdapterEndLiveHeapObservation(invocation->vm_adapter, observation_lease);
    if (release_status != DARTPLANT_OK) return release_status;
    if (status != DARTPLANT_OK) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_FUNCTION_IDENTITY,
                                         DARTPLANT_RESOLVE_REJECTED, status,
                                         DARTPLANT_REJECT_FUNCTION_AMBIGUOUS);
        return status;
    }

    if (invocation->requested_method != nullptr &&
        invocation->requested_method->function != nullptr &&
        invocation->requested_method->function->code_target != nullptr) {
        auto& function = *invocation->requested_method->function;
        if (!function.code_target->MergeEvidence(
                dartplant::MethodCodeSize(invocation->requested_method), probe.code,
                probe.entry_alias_count)) {
            dartplant::SetLastError(
                "live VM Code identity contradicts the existing physical entry certificate");
            dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_CODE_TARGET,
                                             DARTPLANT_RESOLVE_REJECTED, DARTPLANT_PROFILE_MISMATCH,
                                             DARTPLANT_REJECT_ARTIFACT_MISMATCH);
            return DARTPLANT_PROFILE_MISMATCH;
        }
        function.function_object = probe.function;
        function.code_object = probe.code;
        dartplant::DartMethodIdentity live_identity = {
            .library_uri = probe.library_uri,
            .class_name = probe.class_name,
            .function_name = probe.function_name,
            .signature = invocation->requested_method->record.signature,
            .entry_kind = invocation->requested_method->record.entry_kind,
        };
        function.code_target->AddAlias(live_identity);
        if (function.code_target->IsShared() && invocation->hook != nullptr &&
            !invocation->hook->shared_code_opt_in) {
            const DartPlantStatus unhook_status = dartplant::RemoveHook(invocation->hook);
            dartplant::SetLastError(
                unhook_status == DARTPLANT_OK
                    ? "live VM discovered a shared Code entry for a callback hook without explicit shared-code opt-in; the hook was disabled"
                    : "live VM discovered a shared Code entry for a callback hook without explicit shared-code opt-in and the hook could not be disabled");
            dartplant::SetRuntimeDiagnostics(
                runtime, DARTPLANT_RESOLVE_CODE_TARGET, DARTPLANT_RESOLVE_REJECTED,
                DARTPLANT_SHARED_CODE_ENTRY, DARTPLANT_REJECT_CODE_TARGET_AMBIGUOUS);
            return DARTPLANT_SHARED_CODE_ENTRY;
        }
    }

    dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM, DARTPLANT_RESOLVE_RESOLVED,
                                     DARTPLANT_OK);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_bootstrap_live_vm(DartPlantRuntime* runtime,
                                                    const DartPlantLiveVmBootstrapOptions* options,
                                                    DartPlantLiveVmBootstrapInfo* out_info) {
    if (runtime == nullptr ||
        (options != nullptr && options->struct_size < sizeof(DartPlantLiveVmBootstrapOptions)) ||
        (out_info != nullptr && out_info->struct_size < sizeof(DartPlantLiveVmBootstrapInfo))) {
        dartplant::SetLastError("runtime cold-bootstrap arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    dartplant::SetLastError("process-sampled live VM bootstrap has no moving-GC observation lease");
    return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
}

DartPlantStatus dartplant_runtime_bootstrap_live_vm_from_arm64_registers(
    DartPlantRuntime* runtime, const DartPlantLiveVmArm64Registers* registers,
    DartPlantLiveVmBootstrapInfo* out_info) {
    (void) runtime;
    (void) registers;
    (void) out_info;
    dartplant::SetLastError(
        "register live VM bootstrap requires an exact moving-GC observation adapter");
    return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
}

DartPlantStatus dartplant_runtime_bootstrap_live_vm_from_arm64_registers_with_adapter(
    DartPlantRuntime* runtime, const DartPlantLiveVmArm64Registers* registers,
    DartPlantVmAdapter* adapter, DartPlantLiveVmBootstrapInfo* out_info) {
    if (runtime == nullptr || registers == nullptr || adapter == nullptr ||
        registers->struct_size < sizeof(DartPlantLiveVmArm64Registers) ||
        (out_info != nullptr && out_info->struct_size < sizeof(DartPlantLiveVmBootstrapInfo))) {
        dartplant::SetLastError("runtime register-bootstrap arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                     DARTPLANT_RESOLVE_IN_PROGRESS, DARTPLANT_OK);

    dartplant::FlutterSnapshotSource snapshot;
    std::optional<dartplant::ModuleImage> app_module;
    uint64_t generation = 0;
    {
        std::lock_guard lock(runtime->mutex);
        if (!runtime->profile_matched ||
            !dartplant::RuntimeIsolateGroup(runtime).snapshot.has_value() ||
            !dartplant::RuntimeIsolateGroup(runtime).app_module.has_value()) {
            dartplant::SetRuntimeDiagnostics(
                runtime, DARTPLANT_RESOLVE_LIVE_VM, DARTPLANT_RESOLVE_REJECTED,
                DARTPLANT_RUNTIME_NOT_READY, DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE);
            dartplant::SetLastError("runtime images/snapshot are not ready for register bootstrap");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        snapshot = *dartplant::RuntimeIsolateGroup(runtime).snapshot;
        app_module = dartplant::RuntimeIsolateGroup(runtime).app_module;
        generation = runtime->generation->load(std::memory_order_acquire);
    }

    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    dartplant::FillSnapshotInfo(snapshot, &snapshot_info);
    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    void* observation_lease = nullptr;
    DartPlantStatus status =
        dartplant::VmAdapterBeginLiveHeapObservation(adapter, registers->thr, &observation_lease);
    if (status != DARTPLANT_OK) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, status,
                                         DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE);
        return status;
    }
    status = dartplant_live_vm_context_from_arm64_registers(&snapshot_info, registers, &context);
    if (status != DARTPLANT_OK) {
        const DartPlantStatus release_status =
            dartplant::VmAdapterEndLiveHeapObservation(adapter, observation_lease);
        if (release_status != DARTPLANT_OK) return release_status;
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, status,
                                         DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE);
        return status;
    }

    DartPlantStatus index_status = DARTPLANT_OK;
    {
        std::lock_guard lock(runtime->mutex);
        if (runtime->generation->load(std::memory_order_acquire) != generation ||
            !dartplant::SameModuleIdentity(dartplant::RuntimeIsolateGroup(runtime).app_module,
                                           app_module) ||
            !dartplant::SameSnapshotIdentity(dartplant::RuntimeIsolateGroup(runtime).snapshot,
                                             snapshot)) {
            dartplant::SetRuntimeDiagnostics(
                runtime, DARTPLANT_RESOLVE_LIVE_VM, DARTPLANT_RESOLVE_REJECTED,
                DARTPLANT_RUNTIME_NOT_READY, DARTPLANT_REJECT_STALE_GENERATION);
            dartplant::SetLastError("runtime incarnation changed during register bootstrap");
            index_status = DARTPLANT_RUNTIME_NOT_READY;
        } else {
            index_status = dartplant::BuildLiveIndexForContext(
                runtime, context, snapshot, registers->null_value, adapter, observation_lease);
        }
        if (index_status != DARTPLANT_OK) {
            dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_FUNCTION_IDENTITY,
                                             DARTPLANT_RESOLVE_REJECTED, index_status,
                                             DARTPLANT_REJECT_FUNCTION_AMBIGUOUS);
        }
    }
    const DartPlantStatus release_status =
        dartplant::VmAdapterEndLiveHeapObservation(adapter, observation_lease);
    if (release_status != DARTPLANT_OK) return release_status;
    if (index_status != DARTPLANT_OK) return index_status;

    if (out_info != nullptr) {
        DartPlantLiveVmBootstrapInfo info{};
        info.struct_size = sizeof(info);
        info.selected_tid = registers->tid;
        info.rounds = 1;
        info.sampled_threads = 1;
        info.captured_contexts = 1;
        info.validated_candidates = 1;
        info.nonzero_thr_samples = registers->thr != 0 ? 1 : 0;
        info.last_candidate_tid = registers->tid;
        info.selected_pc = registers->pc;
        info.last_candidate_pc = registers->pc;
        info.last_candidate_thr = registers->thr;
        info.last_candidate_pp = registers->pp;
        info.last_candidate_heap_bits = registers->heap_bits;
        info.last_candidate_null = registers->null_value;
        *out_info = info;
    }
    dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM, DARTPLANT_RESOLVE_RESOLVED,
                                     DARTPLANT_OK);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_get_function_index_info(
    const DartPlantRuntime* runtime, DartPlantLiveVmFunctionIndexInfo* out_info) {
    if (runtime == nullptr || out_info == nullptr ||
        out_info->struct_size < sizeof(DartPlantLiveVmFunctionIndexInfo)) {
        dartplant::SetLastError("runtime Function index info arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    if (!dartplant::RuntimeIsolateGroup(runtime).live_snapshot_index.has_value()) {
        dartplant::SetLastError("runtime live Function index is not available");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    *out_info = dartplant::RuntimeIsolateGroup(runtime).live_function_index_info;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_get_function_info(const DartPlantRuntime* runtime, uint32_t index,
                                                    DartPlantLiveVmFunctionInfo* out_info) {
    constexpr size_t kLiveVmFunctionInfoV1Size =
        offsetof(DartPlantLiveVmFunctionInfo, entry_alias_counts);
    if (runtime == nullptr || out_info == nullptr ||
        out_info->struct_size < kLiveVmFunctionInfoV1Size) {
        dartplant::SetLastError("runtime Function info arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    if (!dartplant::RuntimeIsolateGroup(runtime).live_snapshot_index.has_value() ||
        index >= dartplant::RuntimeIsolateGroup(runtime).live_function_index_info.function_count) {
        dartplant::SetLastError("runtime Function index position is out of range");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const auto& cached_infos =
        dartplant::RuntimeIsolateGroup(runtime).live_snapshot_index->live_function_infos;
    if (cached_infos.size() !=
        dartplant::RuntimeIsolateGroup(runtime).live_function_index_info.function_count) {
        dartplant::SetLastError("runtime cached live Function index is inconsistent");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const DartPlantLiveVmFunctionInfo& source = cached_infos[index];
    const size_t caller_size = out_info->struct_size;
    const size_t written_size = std::min(caller_size, sizeof(source));
    std::memcpy(out_info, &source, written_size);
    out_info->struct_size = static_cast<uint32_t>(written_size);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_register_snapshot_index(
    DartPlantRuntime* runtime, const DartPlantSnapshotIndexInfo* source) {
    if (runtime == nullptr || source == nullptr || source->struct_size < sizeof(*source)) {
        dartplant::SetLastError("runtime snapshot index arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    std::string error;
    auto index = dartplant::BuildSnapshotIndex(*source, &error);
    if (!index.has_value()) {
        dartplant::SetLastError(error.empty() ? "snapshot index is invalid" : error);
        return DARTPLANT_METADATA_INVALID;
    }

    std::lock_guard lock(runtime->mutex);
    if (!runtime->profile_matched ||
        !dartplant::RuntimeIsolateGroup(runtime).snapshot.has_value() ||
        !dartplant::RuntimeIsolateGroup(runtime).app_module.has_value()) {
        dartplant::SetLastError("runtime image is not ready for snapshot index binding");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (dartplant::RuntimeIsolateGroup(runtime).artifact_snapshot_index.has_value()) {
        dartplant::SetLastError(
            "an artifact snapshot index is already bound to this app incarnation");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const DartPlantStatus status = dartplant::BindArtifactSnapshotIndex(
        &*index, *dartplant::RuntimeIsolateGroup(runtime).snapshot,
        *dartplant::RuntimeIsolateGroup(runtime).app_module);
    if (status != DARTPLANT_OK) return status;
    dartplant::RuntimeIsolateGroup(runtime).artifact_snapshot_index = std::move(index);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_get_method_signature(
    const DartPlantRuntime* runtime, const DartPlantMethod* method,
    DartPlantDartFunctionSignatureInfo* out_signature) {
    if (runtime == nullptr || method == nullptr || out_signature == nullptr ||
        out_signature->struct_size < sizeof(DartPlantDartFunctionSignatureInfo)) {
        dartplant::SetLastError("runtime method signature arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    const auto& group = dartplant::RuntimeIsolateGroup(runtime);
    if (runtime->state != DARTPLANT_RUNTIME_READY || !group.live_snapshot_index.has_value()) {
        dartplant::SetLastError("runtime live Function semantic snapshot is not available");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!dartplant::IsCurrentRuntimeMethod(runtime, method) || method->function == nullptr ||
        method->function->source != dartplant::DartFunctionSource::kLiveVm) {
        dartplant::SetLastError("method is stale or has no live Function semantic snapshot");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const auto* semantic = group.live_snapshot_index->FindLiveFunctionSemanticSnapshot(
        method->function->image_id, method->record.library_uri, method->record.class_name,
        method->record.function_name);
    if (semantic == nullptr ||
        semantic->signature.struct_size < sizeof(DartPlantDartFunctionSignatureInfo)) {
        dartplant::SetLastError(
            "FunctionType semantics were not captured inside the live heap observation window");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    *out_signature = semantic->signature;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_get_method_parameter(const DartPlantRuntime* runtime,
                                                       const DartPlantMethod* method,
                                                       uint32_t index,
                                                       DartPlantDartParameterInfo* out_parameter) {
    if (runtime == nullptr || method == nullptr || out_parameter == nullptr ||
        out_parameter->struct_size < sizeof(DartPlantDartParameterInfo)) {
        dartplant::SetLastError("runtime method parameter arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::lock_guard lock(runtime->mutex);
    const auto& group = dartplant::RuntimeIsolateGroup(runtime);
    if (runtime->state != DARTPLANT_RUNTIME_READY || !group.live_snapshot_index.has_value()) {
        dartplant::SetLastError("runtime live Function semantic snapshot is not available");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!dartplant::IsCurrentRuntimeMethod(runtime, method) || method->function == nullptr ||
        method->function->source != dartplant::DartFunctionSource::kLiveVm) {
        dartplant::SetLastError("method is stale or has no live Function semantic snapshot");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const auto* semantic = group.live_snapshot_index->FindLiveFunctionSemanticSnapshot(
        method->function->image_id, method->record.library_uri, method->record.class_name,
        method->record.function_name);
    if (semantic == nullptr) {
        dartplant::SetLastError(
            "FunctionType semantics were not captured inside the live heap observation window");
        return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
    }
    if (index >= semantic->parameters.size()) {
        dartplant::SetLastError("FunctionType parameter index is out of range");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    *out_parameter = semantic->parameters[index];
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_read_global_object_pool_entry(
    const DartPlantRuntime* runtime, uint32_t index, DartPlantObjectPoolEntryInfo* out_entry) {
    if (runtime == nullptr || out_entry == nullptr ||
        out_entry->struct_size < sizeof(DartPlantObjectPoolEntryInfo)) {
        dartplant::SetLastError("runtime ObjectPool entry arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    (void) index;
    dartplant::SetLastError(
        "runtime ObjectPool reads require an exact moving-GC observation window");
    return DARTPLANT_VM_BRIDGE_UNAVAILABLE;
}

DartPlantStatus dartplant_runtime_find_method(DartPlantRuntime* runtime,
                                              const DartPlantMethodQuery* query,
                                              DartPlantMethod** out_method) {
    if (runtime == nullptr || query == nullptr || out_method == nullptr ||
        query->struct_size < sizeof(DartPlantMethodQuery) || query->library_uri == nullptr ||
        query->function_name == nullptr) {
        dartplant::SetLastError("runtime method query is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) {
        dartplant::SetLastError("runtime is closing or destroyed");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    // Artifact binding reads and mutates runtime state, so it must be covered
    // by the same lifetime lease as the lookup itself. Acquiring this after the
    // bind leaves a destroy race before runtime->mutex is first touched.
    const DartPlantStatus artifact_status = dartplant::BindRegisteredArtifactIndexIfReady(runtime);
    if (artifact_status != DARTPLANT_OK) return artifact_status;
    std::unique_lock lock(runtime->mutex);
    dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_FUNCTION_IDENTITY,
                                     DARTPLANT_RESOLVE_IN_PROGRESS, DARTPLANT_OK);
    auto& diagnostics = dartplant::RuntimeDiagnostics(runtime);
    diagnostics.requested_entry_kind = query->entry_kind;
    diagnostics.function_candidate_count = 0;
    diagnostics.code_alias_count = 0;
    diagnostics.selected_entry = 0;
    dartplant::ProjectActiveDiagnostics(runtime);
    if (!dartplant::RuntimeIsolateGroup(runtime).snapshot.has_value() ||
        !dartplant::RuntimeIsolateGroup(runtime).app_module.has_value()) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_SNAPSHOT_IDENTITY,
                                         DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                                         DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE);
        dartplant::SetLastError("runtime app image/snapshot is not ready for method resolution");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!dartplant::CanUseLiveVmForQuery(*query)) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_ENTRY_KIND,
                                         DARTPLANT_RESOLVE_REJECTED, DARTPLANT_METHOD_NOT_FOUND,
                                         query->entry_kind == DARTPLANT_ENTRY_DEFAULT
                                             ? DARTPLANT_REJECT_FUNCTION_NOT_FOUND
                                             : DARTPLANT_REJECT_ENTRY_KIND_UNSUPPORTED);
        dartplant::SetLastError(
            "method query is outside the supported live Function-index identity domain");
        return DARTPLANT_METHOD_NOT_FOUND;
    }
    DartPlantStatus status = DARTPLANT_METHOD_NOT_FOUND;
    if (runtime->state == DARTPLANT_RUNTIME_READY &&
        dartplant::RuntimeIsolateGroup(runtime).live_vm_context.has_value() &&
        dartplant::RuntimeIsolateGroup(runtime).live_snapshot_index.has_value()) {
        status = dartplant::ResolveLiveIndexedRuntimeMethod(
            *dartplant::RuntimeIsolateGroup(runtime).live_snapshot_index,
            dartplant::RuntimeIsolateGroup(runtime).image_set.empty()
                ? nullptr
                : &dartplant::RuntimeIsolateGroup(runtime).image_set,
            *dartplant::RuntimeIsolateGroup(runtime).app_module,
            dartplant::RuntimeIsolateGroup(runtime).entry_targets, *query, runtime->generation,
            runtime->generation->load(std::memory_order_acquire), out_method);
        const auto* root_image = dartplant::RuntimeIsolateGroup(runtime).image_set.Root();
        const bool live_method_is_root =
            status == DARTPLANT_OK && *out_method != nullptr &&
            (*out_method)->function != nullptr &&
            ((*out_method)->function->image_id == 0 ||
             (root_image != nullptr && (*out_method)->function->image_id == root_image->id));
        if (live_method_is_root &&
            dartplant::RuntimeIsolateGroup(runtime).artifact_snapshot_index.has_value()) {
            const DartPlantStatus merge_status =
                dartplant::MergeValidatedArtifactIdentityIntoLiveMethod(
                    *dartplant::RuntimeIsolateGroup(runtime).artifact_snapshot_index, *query,
                    *out_method);
            if (merge_status != DARTPLANT_OK) {
                dartplant_release_method(*out_method);
                *out_method = nullptr;
                status = merge_status;
            }
        }
    }
    if (status == DARTPLANT_METHOD_NOT_FOUND &&
        dartplant::RuntimeIsolateGroup(runtime).artifact_snapshot_index.has_value()) {
        const auto* root_image = dartplant::RuntimeIsolateGroup(runtime).image_set.Root();
        status = dartplant::ResolveArtifactIndexedRuntimeMethod(
            *dartplant::RuntimeIsolateGroup(runtime).artifact_snapshot_index,
            *dartplant::RuntimeIsolateGroup(runtime).snapshot,
            *dartplant::RuntimeIsolateGroup(runtime).app_module, root_image,
            dartplant::RuntimeIsolateGroup(runtime).entry_targets, *query, runtime->generation,
            runtime->generation->load(std::memory_order_acquire), out_method);
    }
    if (status == DARTPLANT_METHOD_NOT_FOUND && runtime->state != DARTPLANT_RUNTIME_READY) {
        dartplant::SetLastError(
            "method is not present in the exact artifact index and the live VM is not ready");
        status = DARTPLANT_RUNTIME_NOT_READY;
    }
    if (status == DARTPLANT_OK && *out_method != nullptr && (*out_method)->function != nullptr) {
        (*out_method)->function->isolate_group_identity =
            dartplant::RuntimeIsolateGroup(runtime).isolate_group_identity;
    }
    if (status != DARTPLANT_OK) {
        const DartPlantResolveRejectReason reason =
            status == DARTPLANT_AMBIGUOUS_METHOD    ? DARTPLANT_REJECT_FUNCTION_AMBIGUOUS
            : status == DARTPLANT_PROFILE_MISMATCH  ? DARTPLANT_REJECT_ARTIFACT_MISMATCH
            : status == DARTPLANT_RUNTIME_NOT_READY ? DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE
                                                    : DARTPLANT_REJECT_FUNCTION_NOT_FOUND;
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_FUNCTION_IDENTITY,
                                         DARTPLANT_RESOLVE_REJECTED, status, reason);
    }
    lock.unlock();
    if (status != DARTPLANT_OK) return status;
    {
        std::lock_guard diagnostics_lock(runtime->mutex);
        auto& diagnostics = dartplant::RuntimeDiagnostics(runtime);
        diagnostics.function_candidate_count = 1;
        diagnostics.code_alias_count =
            (*out_method)->function != nullptr && (*out_method)->function->code_target != nullptr
                ? (*out_method)->function->code_target->KnownAliasCount()
                : 0;
        diagnostics.selected_entry = dartplant_method_runtime_address(*out_method);
        dartplant::ProjectActiveDiagnostics(runtime);
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_CODE_TARGET,
                                         DARTPLANT_RESOLVE_RESOLVED, DARTPLANT_OK);
    }
    const DartPlantStatus evidence_status =
        dartplant::BindRegisteredCompilerEvidenceIfPresent(runtime, *out_method);
    if (evidence_status != DARTPLANT_OK) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_ABI_EVIDENCE,
                                         DARTPLANT_RESOLVE_REJECTED, evidence_status,
                                         DARTPLANT_REJECT_ABI_CONFLICT);
        dartplant_release_method(*out_method);
        *out_method = nullptr;
        return evidence_status;
    }
    dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_COMPLETE,
                                     DARTPLANT_RESOLVE_RESOLVED, DARTPLANT_OK);
    return DARTPLANT_OK;
}

}  // extern "C"
