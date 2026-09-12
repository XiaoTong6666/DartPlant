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

bool SameSemanticContext(const DartPlantLiveVmContext& left, uint64_t left_null,
                         uint64_t left_bool_true, uint64_t left_bool_false,
                         const DartPlantLiveVmContext& right, uint64_t right_null,
                         uint64_t right_bool_true, uint64_t right_bool_false) {
    return left.profile_version == right.profile_version && left.isolate == right.isolate &&
           left.isolate_group == right.isolate_group && left.class_table == right.class_table &&
           left.cached_class_table_table == right.cached_class_table_table &&
           left.object_store == right.object_store && left.heap_base == right.heap_base &&
           left.pp == right.pp && left.global_object_pool == right.global_object_pool &&
           left.object_pool_length == right.object_pool_length && left_null == right_null &&
           left_bool_true == right_bool_true && left_bool_false == right_bool_false;
}

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
            owning_image->live_entry_count == 0) {
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

    auto code_target = entry_targets.GetOrCreate(
        record->runtime_entry, static_cast<uint32_t>(record->code_size), record->code_object,
        record->entry_alias_count, DARTPLANT_CODE_IDENTITY_UNKNOWN,
        static_cast<uintptr_t>(record->code_payload_start), record->code_instructions_length,
        image_id);
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
    function->function_object = record->function_object;
    function->code_object = record->code_object;
    function->source = DartFunctionSource::kLiveVm;
    function->function_kind = record->function_kind;
    function->runtime_profile_version = profile == nullptr ? 0 : profile->live_vm.profile_version;
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
    uint64_t image_id, DartEntryTargetRegistry& entry_targets, const DartPlantMethodQuery& query,
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

    auto code_target = entry_targets.GetOrCreate(
        record->runtime_entry, static_cast<uint32_t>(record->code_size), 0,
        std::max<uint32_t>(1, record->entry_alias_count), record->code_identity_proof,
        static_cast<uintptr_t>(record->code_payload_start), record->code_instructions_length,
        image_id);
    if (code_target == nullptr) {
        SetLastError("artifact snapshot index produced an invalid entry target");
        return DARTPLANT_METHOD_NOT_FOUND;
    }
    auto function = std::make_shared<DartFunctionHandle>();
    function->identity = MethodIdentityFromRecord(method_record);
    function->image_id = image_id;
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

bool EqualsIgnoreCaseAscii(const std::string& left, const std::string& right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
                                                     [](unsigned char a, unsigned char b) {
                                                         return std::tolower(a) == std::tolower(b);
                                                     });
}

bool IsCurrentRuntimeMethod(const DartPlantRuntime* runtime, const DartPlantMethod* method) {
    return runtime != nullptr && method != nullptr &&
           method->runtime_generation == runtime->generation &&
           method->expected_runtime_generation ==
               runtime->generation->load(std::memory_order_acquire);
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
    if (!runtime->profile_matched || !runtime->snapshot.has_value() ||
        !runtime->selected_app_module.has_value()) {
        SetLastError("runtime image is not ready for registered artifact binding");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const DartPlantStatus status = BindArtifactSnapshotIndex(
        &*index, *runtime->snapshot, *runtime->selected_app_module,
        runtime->artifact_snapshot_index.has_value() ? &*runtime->artifact_snapshot_index
                                                     : nullptr);
    if (status != DARTPLANT_OK) return status;
    runtime->artifact_snapshot_index = std::move(index);
    runtime->bound_artifact_snapshot_generation = registry_generation;
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
    if (stage == DARTPLANT_RESOLVE_MODULE_SELECTION) {
        runtime->diagnostics.requested_entry_kind = DARTPLANT_ENTRY_DEFAULT;
        runtime->diagnostics.module_candidate_count = 0;
        runtime->diagnostics.function_candidate_count = 0;
        runtime->diagnostics.code_alias_count = 0;
        runtime->diagnostics.abi_provider_count = 0;
        runtime->diagnostics.structural_candidate_count = 0;
        runtime->diagnostics.structural_relation_count = 0;
        runtime->diagnostics.selected_entry = 0;
    } else if (stage == DARTPLANT_RESOLVE_FUNCTION_IDENTITY &&
               outcome == DARTPLANT_RESOLVE_IN_PROGRESS) {
        runtime->diagnostics.function_candidate_count = 0;
        runtime->diagnostics.code_alias_count = 0;
        runtime->diagnostics.abi_provider_count = 0;
        runtime->diagnostics.structural_candidate_count = 0;
        runtime->diagnostics.structural_relation_count = 0;
        runtime->diagnostics.selected_entry = 0;
    } else if (stage == DARTPLANT_RESOLVE_ABI_EVIDENCE &&
               outcome == DARTPLANT_RESOLVE_IN_PROGRESS) {
        runtime->diagnostics.abi_provider_count = 0;
    }
    runtime->diagnostics.struct_size = sizeof(runtime->diagnostics);
    runtime->diagnostics.stage = stage;
    runtime->diagnostics.outcome = outcome;
    runtime->diagnostics.reject_reason = reject_reason;
    runtime->diagnostics.status = status;
    runtime->diagnostics.runtime_generation = runtime->generation->load(std::memory_order_acquire);
}

DartPlantStatus RefreshRuntimeModules(DartPlantRuntime* runtime,
                                      const std::vector<ModuleImage>& modules) {
    if (runtime == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    std::lock_guard lock(runtime->mutex);

    SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_MODULE_SELECTION,
                          DARTPLANT_RESOLVE_IN_PROGRESS, DARTPLANT_OK);

    const auto old_app = runtime->selected_app_module;
    const auto old_dart_runtime = runtime->selected_runtime_module;
    const ModuleSelection app_selection = SelectProfileModule(
        modules, runtime->profile.app_module_name, runtime->profile.app_build_id);
    const ModuleSelection dart_runtime_selection =
        SelectProfileModule(modules, runtime->profile.runtime_module_name,
                            runtime->profile.runtime_build_id, runtime->engine_anchor);
    runtime->diagnostics.module_candidate_count =
        app_selection.candidate_count + dart_runtime_selection.candidate_count;
    if (app_selection.state == ModuleSelectionState::kAmbiguous ||
        dart_runtime_selection.state == ModuleSelectionState::kAmbiguous) {
        const bool old_app_mapping_present = ContainsModuleIdentity(modules, old_app);
        runtime->generation->fetch_add(1, std::memory_order_acq_rel);
        DartPlantStatus invalidation_status = DARTPLANT_OK;
        if (old_app.has_value() && !old_app_mapping_present) {
            RetireRuntimeHooks(runtime->generation);
        } else {
            invalidation_status = InvalidateRuntimeHooks(runtime->generation);
        }
        runtime->entry_targets.Clear();
        runtime->abi_evidence.clear();
        runtime->snapshot.reset();
        runtime->live_vm_context.reset();
        runtime->live_vm_core_candidates.clear();
        runtime->live_vm_null_value = 0;
        runtime->live_vm_bool_true_value = 0;
        runtime->live_vm_bool_false_value = 0;
        runtime->live_snapshot_index.reset();
        runtime->artifact_snapshot_index.reset();
        runtime->bound_artifact_snapshot_generation = 0;
        runtime->live_function_index_info = {};
        runtime->image_set.Clear();
        runtime->modules = modules;
        runtime->selected_app_module.reset();
        runtime->selected_runtime_module.reset();
        runtime->profile_matched = false;
        runtime->state = DARTPLANT_RUNTIME_FAILED;
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
    const bool old_app_mapping_present = ContainsModuleIdentity(modules, old_app);

    std::optional<FlutterSnapshotSource> snapshot;
    std::string snapshot_error;
    if (!app_changed && runtime->snapshot.has_value()) {
        snapshot = runtime->snapshot;
    } else if (new_app.has_value()) {
        snapshot = DiscoverFlutterSnapshot(*new_app, &snapshot_error);
    }
    const bool snapshot_changed = !SameSnapshotIdentity(runtime->snapshot, snapshot);
    std::string image_set_error;
    auto image_set = BuildRuntimeImageSet(modules, new_app, snapshot,
                                          runtime->generation->load(std::memory_order_acquire),
                                          &image_set_error);
    if (!image_set.has_value()) {
        runtime->state = DARTPLANT_RUNTIME_FAILED;
        SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_SNAPSHOT_IDENTITY,
                              DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                              DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE);
        SetLastError(image_set_error.empty() ? "runtime image set is malformed" : image_set_error);
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const bool image_set_changed = !runtime->image_set.SameIdentity(*image_set);
    const bool relevant_identity_changed =
        app_changed || dart_runtime_changed || snapshot_changed || image_set_changed;

    runtime->modules = modules;
    // Publish the selected current incarnations before invalidation can fail,
    // so FAILED state reporting never describes an unloaded dependency.
    runtime->selected_app_module = new_app;
    runtime->selected_runtime_module = new_dart_runtime;
    DartPlantStatus hook_invalidation_status = DARTPLANT_OK;
    if (relevant_identity_changed) {
        runtime->generation->fetch_add(1, std::memory_order_acq_rel);
        image_set->BindGeneration(runtime->generation->load(std::memory_order_acquire));
        if (app_changed && old_app.has_value() && !old_app_mapping_present) {
            // The old app incarnation is confirmed absent. Its target address
            // may be unmapped or reused, so only retire registry ownership.
            RetireRuntimeHooks(runtime->generation);
        } else {
            hook_invalidation_status = InvalidateRuntimeHooks(runtime->generation);
        }
        runtime->entry_targets.Clear();
        runtime->abi_evidence.clear();
        runtime->snapshot = std::move(snapshot);
        runtime->live_vm_context.reset();
        runtime->live_vm_core_candidates.clear();
        runtime->live_vm_null_value = 0;
        runtime->live_vm_bool_true_value = 0;
        runtime->live_vm_bool_false_value = 0;
        runtime->live_snapshot_index.reset();
        runtime->artifact_snapshot_index.reset();
        runtime->bound_artifact_snapshot_generation = 0;
        runtime->live_function_index_info = {};
        runtime->image_set = std::move(*image_set);
    } else if (runtime->image_set.empty() && !image_set->empty()) {
        runtime->image_set = std::move(*image_set);
    }

    if (hook_invalidation_status != DARTPLANT_OK) {
        runtime->state = DARTPLANT_RUNTIME_FAILED;
        return hook_invalidation_status;
    }

    runtime->profile_matched = new_app.has_value() && new_dart_runtime.has_value();
    if (!runtime->profile_matched) {
        runtime->state = DARTPLANT_RUNTIME_CREATED;
        SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_MODULE_SELECTION,
                              DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                              DARTPLANT_REJECT_MODULE_NOT_FOUND);
        SetLastError("runtime profile modules are not loaded or mismatch");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_SNAPSHOT_IDENTITY,
                          DARTPLANT_RESOLVE_IN_PROGRESS, DARTPLANT_OK);
    if (!runtime->snapshot.has_value()) {
        runtime->state = DARTPLANT_RUNTIME_FAILED;
        SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_SNAPSHOT_IDENTITY,
                              DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                              DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE);
        SetLastError(snapshot_error.empty()
                         ? "loaded app module has no usable Flutter snapshot source"
                         : snapshot_error);
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (relevant_identity_changed || runtime->state != DARTPLANT_RUNTIME_READY) {
        runtime->state = DARTPLANT_RUNTIME_IMAGES_READY;
    }
    SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_SNAPSHOT_IDENTITY, DARTPLANT_RESOLVE_RESOLVED,
                          DARTPLANT_OK);
    ClearLastError();
    return DARTPLANT_OK;
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
                    RefreshRuntimeModules(operation.registration->runtime, modules);
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
                                         uint64_t validated_null_value) {
    if (runtime == nullptr) return DARTPLANT_INVALID_ARGUMENT;
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
    resolver_input.canonical_null = validated_null_value;
    resolver_input.registers = {.available = false};
    resolver_input.modules = &runtime->modules;
    const vm_abi::ResolverResult resolver = vm_abi::ResolveVerifiedBinding(resolver_input);
    if (!resolver.passed || resolver.binding.core.representative == nullptr) {
        SetLastError(resolver.distinct_abis > 1
                         ? "live VM runtime-root capability is ambiguous across candidate profiles"
                         : "live VM runtime-root capability rejected every candidate profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    const RuntimeProfileRecord* selected_profile = resolver.binding.core.representative;
    const DartPlantLiveVmProfile profile = selected_profile->live_vm;
    DartPlantStatus status = DARTPLANT_OK;
    std::optional<uint32_t> root_program_hash;
    if (runtime->image_set.size() > 1) {
        uint32_t program_hash = 0;
        status = ReadLiveVmRootProgramHashForProfile(context, *selected_profile, &program_hash);
        if (status != DARTPLANT_OK) return status;
        root_program_hash = program_hash;
    }
    uint64_t bool_true_value = 0;
    uint64_t bool_false_value = 0;
    status = ResolveLiveVmCanonicalBoolRoots(context, profile, &bool_true_value, &bool_false_value);
    if (status != DARTPLANT_OK) return status;

    DartPlantLiveVmFunctionIndexInfo index_info{};
    index_info.struct_size = sizeof(index_info);
    std::string error;
    std::optional<SnapshotIndex> index;
    if (!runtime->image_set.empty()) {
        std::vector<LiveVmInstructionImage> instruction_images;
        instruction_images.reserve(runtime->image_set.size());
        for (const auto& image : runtime->image_set.images()) {
            LiveVmInstructionImage descriptor{};
            descriptor.runtime_image_id = image.id;
            descriptor.loading_unit_id = image.loading_unit_id;
            descriptor.snapshot.struct_size = sizeof(descriptor.snapshot);
            FillSnapshotInfo(image.snapshot, &descriptor.snapshot);
            instruction_images.push_back(descriptor);
        }
        index = BuildLiveSnapshotIndexForImages(context, instruction_images, *selected_profile,
                                                &index_info, &error);
    } else {
        // Synthetic/legacy callers that seed runtime fields directly retain the
        // single-image contract until they opt into RuntimeImageSet.
        index =
            BuildLiveSnapshotIndex(context, snapshot_info, *selected_profile, &index_info, &error);
    }
    if (!index.has_value()) {
        SetLastError(error.empty() ? "failed to build live Function index" : error);
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    std::optional<RuntimeImageSet> semantically_bound_images;
    if (!runtime->image_set.empty()) {
        RuntimeImageSet rebound;
        if (!BindLiveSnapshotImageSemantics(*index, runtime->image_set, &rebound, &error)) {
            SetLastError(error.empty() ? "live Function image semantics are inconsistent" : error);
            return DARTPLANT_PROFILE_MISMATCH;
        }
        if (root_program_hash.has_value() && !rebound.BindDeferredProgramHash(*root_program_hash)) {
            SetLastError(
                "deferred runtime image program hash disagrees with the live isolate group");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        semantically_bound_images = std::move(rebound);
    }
    const bool semantic_context_changed =
        runtime->live_vm_context.has_value() &&
        !SameSemanticContext(*runtime->live_vm_context, runtime->live_vm_null_value,
                             runtime->live_vm_bool_true_value, runtime->live_vm_bool_false_value,
                             context, validated_null_value, bool_true_value, bool_false_value);
    if (semantic_context_changed) {
        runtime->generation->fetch_add(1, std::memory_order_acq_rel);
        const DartPlantStatus invalidation_status = InvalidateRuntimeHooks(runtime->generation);
        runtime->entry_targets.Clear();
        runtime->abi_evidence.clear();
        if (invalidation_status != DARTPLANT_OK) {
            runtime->state = DARTPLANT_RUNTIME_FAILED;
            return invalidation_status;
        }
    }
    if (semantically_bound_images.has_value()) {
        semantically_bound_images->BindGeneration(
            runtime->generation->load(std::memory_order_acquire));
        runtime->image_set = std::move(*semantically_bound_images);
    }
    runtime->live_vm_context = context;
    runtime->live_vm_core_candidates = resolver.binding.core.candidates.profiles;
    runtime->live_vm_null_value = validated_null_value;
    runtime->live_vm_bool_true_value = bool_true_value;
    runtime->live_vm_bool_false_value = bool_false_value;
    runtime->live_snapshot_index = std::move(index);
    runtime->live_function_index_info = index_info;
    runtime->state = DARTPLANT_RUNTIME_READY;
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
    bool app_mapping_is_current = false;
    {
        std::lock_guard lock(runtime->mutex);
        app_mapping_is_current =
            dartplant::ContainsModuleIdentity(modules, runtime->selected_app_module);
    }
    runtime->generation->fetch_add(1, std::memory_order_acq_rel);
    if (app_mapping_is_current) {
        dartplant::InvalidateRuntimeHooks(runtime->generation);
    } else {
        dartplant::RetireRuntimeHooks(runtime->generation);
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
    return dartplant::RefreshRuntimeModules(runtime, modules);
}

DartPlantStatus dartplant_runtime_bind_engine_anchor(DartPlantRuntime* runtime,
                                                     const void* engine_address) {
    if (runtime == nullptr) {
        dartplant::SetLastError("runtime is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) return DARTPLANT_RUNTIME_NOT_READY;
    {
        std::lock_guard lock(runtime->mutex);
        runtime->engine_anchor = reinterpret_cast<uintptr_t>(engine_address);
    }
    return dartplant::RefreshRuntimeModules(runtime, dartplant::EnumerateModules());
}

DartPlantStatus dartplant_runtime_on_module_loaded(DartPlantRuntime* runtime, const char*, void*) {
    return dartplant_runtime_refresh_modules(runtime);
}

DartPlantStatus dartplant_runtime_on_module_unloading(DartPlantRuntime* runtime, const char*,
                                                      void*) {
    if (runtime == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) return DARTPLANT_RUNTIME_NOT_READY;
    runtime->generation->fetch_add(1, std::memory_order_acq_rel);
    const DartPlantStatus status = dartplant::InvalidateRuntimeHooks(runtime->generation);
    std::lock_guard lock(runtime->mutex);
    runtime->entry_targets.Clear();
    runtime->abi_evidence.clear();
    runtime->live_vm_context.reset();
    runtime->live_snapshot_index.reset();
    runtime->artifact_snapshot_index.reset();
    runtime->bound_artifact_snapshot_generation = 0;
    runtime->image_set.Clear();
    runtime->state = status == DARTPLANT_OK ? DARTPLANT_RUNTIME_CREATED : DARTPLANT_RUNTIME_FAILED;
    return status;
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
    out_info->loaded_module_count = static_cast<uint32_t>(runtime->modules.size());
    out_info->app_module_loaded = runtime->selected_app_module.has_value() ? 1 : 0;
    out_info->runtime_module_loaded = runtime->selected_runtime_module.has_value() ? 1 : 0;
    out_info->live_function_index_ready = runtime->live_snapshot_index.has_value() ? 1 : 0;
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
    if (runtime->image_set.size() > UINT32_MAX) {
        dartplant::SetLastError("runtime image count exceeds public ABI range");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    *out_count = static_cast<uint32_t>(runtime->image_set.size());
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_get_image_info(const DartPlantRuntime* runtime, uint32_t index,
                                                 DartPlantRuntimeImageInfo* out_info) {
    if (runtime == nullptr || out_info == nullptr || out_info->struct_size < sizeof(*out_info)) {
        dartplant::SetLastError("runtime image info arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto operation = dartplant::AcquireRuntimeOperation(runtime);
    if (!operation) return DARTPLANT_RUNTIME_NOT_READY;
    std::lock_guard lock(runtime->mutex);
    if (index >= runtime->image_set.size()) {
        dartplant::SetLastError("runtime image index is out of range");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const auto& image = runtime->image_set.images()[index];
    *out_info = {};
    out_info->struct_size = sizeof(*out_info);
    out_info->kind = image.kind == dartplant::RuntimeImageKind::kRoot
                         ? DARTPLANT_RUNTIME_IMAGE_ROOT
                         : DARTPLANT_RUNTIME_IMAGE_DEFERRED;
    out_info->image_id = image.id;
    out_info->runtime_generation = image.runtime_generation;
    out_info->loading_unit_id = image.loading_unit_id;
    out_info->module_name = image.module.name.c_str();
    out_info->module_path = image.module.path.c_str();
    out_info->module_build_id = image.module.build_id.c_str();
    out_info->snapshot_hash = image.snapshot.snapshot_hash.c_str();
    out_info->snapshot_features = image.snapshot.snapshot_features.c_str();
    out_info->profile_name = image.snapshot.profile_name.c_str();
    out_info->isolate_instructions_va = image.snapshot.isolate_instructions_va;
    out_info->isolate_instructions_size = image.snapshot.isolate_instructions_size;
    out_info->isolate_instructions_runtime = image.snapshot.isolate_instructions_runtime;
    out_info->live_entry_count = image.live_entry_count;
    out_info->live_semantic_bound = image.live_entry_count != 0 ? 1 : 0;
    out_info->has_deferred_program_hash = image.snapshot.deferred_program_hash.has_value() ? 1 : 0;
    out_info->deferred_program_hash_vm_bound = image.deferred_program_hash_vm_bound ? 1 : 0;
    out_info->deferred_program_hash = image.snapshot.deferred_program_hash.value_or(0);
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
    *out_diagnostics = runtime->diagnostics;
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
    if (!runtime->snapshot.has_value()) {
        dartplant::SetLastError("Flutter snapshot source is not available");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    const auto& snapshot = *runtime->snapshot;
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
    if (!runtime->snapshot.has_value()) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, DARTPLANT_RUNTIME_NOT_READY,
                                         DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE);
        dartplant::SetLastError("Flutter snapshot source is not available for live VM capture");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (invocation->requested_method == nullptr ||
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

    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    dartplant::FillSnapshotInfo(*runtime->snapshot, &snapshot_info);
    DartPlantLiveVmProbeInfo probe{};
    probe.struct_size = sizeof(probe);
    DartPlantStatus status = dartplant_live_vm_probe_invocation(invocation, &snapshot_info, &probe);
    if (status != DARTPLANT_OK) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, status,
                                         DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE);
        return status;
    }

    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    status = dartplant_live_vm_context_from_probe(&probe, &context);
    if (status != DARTPLANT_OK) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, status,
                                         DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE);
        return status;
    }
    const dartplant::RuntimeProfileRecord* probe_profile =
        dartplant::FindRuntimeProfileByVersion(probe.profile_version);
    if (probe_profile == nullptr || probe_profile->live_vm.null_register >= 31) {
        dartplant::SetLastError("live VM probe selected an invalid candidate profile");
        return DARTPLANT_PROFILE_MISMATCH;
    }
    status = dartplant::BuildLiveIndexForContext(
        runtime, context, *runtime->snapshot,
        invocation->context->x[probe_profile->live_vm.null_register]);
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
        if (!runtime->profile_matched || !runtime->snapshot.has_value() ||
            !runtime->selected_app_module.has_value()) {
            dartplant::SetRuntimeDiagnostics(
                runtime, DARTPLANT_RESOLVE_LIVE_VM, DARTPLANT_RESOLVE_REJECTED,
                DARTPLANT_RUNTIME_NOT_READY, DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE);
            dartplant::SetLastError("runtime images/snapshot are not ready for cold bootstrap");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        snapshot = *runtime->snapshot;
        app_module = runtime->selected_app_module;
        generation = runtime->generation->load(std::memory_order_acquire);
    }

    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    dartplant::FillSnapshotInfo(snapshot, &snapshot_info);
    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    DartPlantLiveVmBootstrapInfo bootstrap_info{};
    bootstrap_info.struct_size = sizeof(bootstrap_info);
    const DartPlantStatus status =
        dartplant_live_vm_bootstrap_process(&snapshot_info, options, &context, &bootstrap_info);
    if (out_info != nullptr) *out_info = bootstrap_info;
    if (status != DARTPLANT_OK) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, status,
                                         DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE);
        return status;
    }

    {
        std::lock_guard lock(runtime->mutex);
        if (runtime->generation->load(std::memory_order_acquire) != generation ||
            !dartplant::SameModuleIdentity(runtime->selected_app_module, app_module) ||
            !dartplant::SameSnapshotIdentity(runtime->snapshot, snapshot)) {
            dartplant::SetRuntimeDiagnostics(
                runtime, DARTPLANT_RESOLVE_LIVE_VM, DARTPLANT_RESOLVE_REJECTED,
                DARTPLANT_RUNTIME_NOT_READY, DARTPLANT_REJECT_STALE_GENERATION);
            dartplant::SetLastError("runtime incarnation changed during cold bootstrap");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        const DartPlantStatus index_status = dartplant::BuildLiveIndexForContext(
            runtime, context, snapshot, bootstrap_info.last_candidate_null);
        if (index_status != DARTPLANT_OK) {
            dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_FUNCTION_IDENTITY,
                                             DARTPLANT_RESOLVE_REJECTED, index_status,
                                             DARTPLANT_REJECT_FUNCTION_AMBIGUOUS);
            return index_status;
        }
    }
    dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM, DARTPLANT_RESOLVE_RESOLVED,
                                     DARTPLANT_OK);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_bootstrap_live_vm_from_arm64_registers(
    DartPlantRuntime* runtime, const DartPlantLiveVmArm64Registers* registers,
    DartPlantLiveVmBootstrapInfo* out_info) {
    if (runtime == nullptr || registers == nullptr ||
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
        if (!runtime->profile_matched || !runtime->snapshot.has_value() ||
            !runtime->selected_app_module.has_value()) {
            dartplant::SetRuntimeDiagnostics(
                runtime, DARTPLANT_RESOLVE_LIVE_VM, DARTPLANT_RESOLVE_REJECTED,
                DARTPLANT_RUNTIME_NOT_READY, DARTPLANT_REJECT_SNAPSHOT_UNAVAILABLE);
            dartplant::SetLastError("runtime images/snapshot are not ready for register bootstrap");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        snapshot = *runtime->snapshot;
        app_module = runtime->selected_app_module;
        generation = runtime->generation->load(std::memory_order_acquire);
    }

    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    dartplant::FillSnapshotInfo(snapshot, &snapshot_info);
    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    const DartPlantStatus status =
        dartplant_live_vm_context_from_arm64_registers(&snapshot_info, registers, &context);
    if (status != DARTPLANT_OK) {
        dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_LIVE_VM,
                                         DARTPLANT_RESOLVE_REJECTED, status,
                                         DARTPLANT_REJECT_LIVE_VM_UNAVAILABLE);
        return status;
    }

    {
        std::lock_guard lock(runtime->mutex);
        if (runtime->generation->load(std::memory_order_acquire) != generation ||
            !dartplant::SameModuleIdentity(runtime->selected_app_module, app_module) ||
            !dartplant::SameSnapshotIdentity(runtime->snapshot, snapshot)) {
            dartplant::SetRuntimeDiagnostics(
                runtime, DARTPLANT_RESOLVE_LIVE_VM, DARTPLANT_RESOLVE_REJECTED,
                DARTPLANT_RUNTIME_NOT_READY, DARTPLANT_REJECT_STALE_GENERATION);
            dartplant::SetLastError("runtime incarnation changed during register bootstrap");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        const DartPlantStatus index_status =
            dartplant::BuildLiveIndexForContext(runtime, context, snapshot, registers->null_value);
        if (index_status != DARTPLANT_OK) {
            dartplant::SetRuntimeDiagnostics(runtime, DARTPLANT_RESOLVE_FUNCTION_IDENTITY,
                                             DARTPLANT_RESOLVE_REJECTED, index_status,
                                             DARTPLANT_REJECT_FUNCTION_AMBIGUOUS);
            return index_status;
        }
    }

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
    if (!runtime->live_snapshot_index.has_value()) {
        dartplant::SetLastError("runtime live Function index is not available");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    *out_info = runtime->live_function_index_info;
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
    if (!runtime->live_snapshot_index.has_value() ||
        index >= runtime->live_function_index_info.function_count) {
        dartplant::SetLastError("runtime Function index position is out of range");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const auto& cached_infos = runtime->live_snapshot_index->live_function_infos;
    if (cached_infos.size() != runtime->live_function_index_info.function_count) {
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
    if (!runtime->profile_matched || !runtime->snapshot.has_value() ||
        !runtime->selected_app_module.has_value()) {
        dartplant::SetLastError("runtime image is not ready for snapshot index binding");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (runtime->artifact_snapshot_index.has_value()) {
        dartplant::SetLastError(
            "an artifact snapshot index is already bound to this app incarnation");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const DartPlantStatus status = dartplant::BindArtifactSnapshotIndex(
        &*index, *runtime->snapshot, *runtime->selected_app_module);
    if (status != DARTPLANT_OK) return status;
    runtime->artifact_snapshot_index = std::move(index);
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
    if (runtime->state != DARTPLANT_RUNTIME_READY || !runtime->live_vm_context.has_value() ||
        !runtime->snapshot.has_value()) {
        dartplant::SetLastError("runtime LiveVmContext is not available for FunctionType parsing");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!dartplant::IsCurrentRuntimeMethod(runtime, method) || method->function == nullptr ||
        method->function->source != dartplant::DartFunctionSource::kLiveVm ||
        method->function->function_object == 0) {
        dartplant::SetLastError("method is stale or has no live Function object");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    dartplant::FillSnapshotInfo(*runtime->snapshot, &snapshot_info);
    return dartplant_live_vm_read_function_signature(&*runtime->live_vm_context, &snapshot_info,
                                                     method->function->function_object,
                                                     out_signature);
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
    if (runtime->state != DARTPLANT_RUNTIME_READY || !runtime->live_vm_context.has_value() ||
        !runtime->snapshot.has_value()) {
        dartplant::SetLastError("runtime LiveVmContext is not available for FunctionType parsing");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!dartplant::IsCurrentRuntimeMethod(runtime, method) || method->function == nullptr ||
        method->function->source != dartplant::DartFunctionSource::kLiveVm ||
        method->function->function_object == 0) {
        dartplant::SetLastError("method is stale or has no live Function object");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    dartplant::FillSnapshotInfo(*runtime->snapshot, &snapshot_info);
    return dartplant_live_vm_read_function_parameter(&*runtime->live_vm_context, &snapshot_info,
                                                     method->function->function_object, index,
                                                     out_parameter);
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
    std::lock_guard lock(runtime->mutex);
    if (!runtime->live_vm_context.has_value() || !runtime->snapshot.has_value()) {
        dartplant::SetLastError("runtime LiveVmContext is not available for ObjectPool lookup");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    dartplant::FillSnapshotInfo(*runtime->snapshot, &snapshot_info);
    return dartplant_live_vm_read_object_pool_entry(&*runtime->live_vm_context, &snapshot_info,
                                                    runtime->live_vm_context->global_object_pool,
                                                    index, out_entry);
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
    runtime->diagnostics.requested_entry_kind = query->entry_kind;
    runtime->diagnostics.function_candidate_count = 0;
    runtime->diagnostics.code_alias_count = 0;
    runtime->diagnostics.selected_entry = 0;
    if (!runtime->snapshot.has_value() || !runtime->selected_app_module.has_value()) {
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
    if (runtime->state == DARTPLANT_RUNTIME_READY && runtime->live_vm_context.has_value() &&
        runtime->live_snapshot_index.has_value()) {
        status = dartplant::ResolveLiveIndexedRuntimeMethod(
            *runtime->live_snapshot_index,
            runtime->image_set.empty() ? nullptr : &runtime->image_set,
            *runtime->selected_app_module, runtime->entry_targets, *query, runtime->generation,
            runtime->generation->load(std::memory_order_acquire), out_method);
        const auto* root_image = runtime->image_set.Root();
        const bool live_method_is_root =
            status == DARTPLANT_OK && *out_method != nullptr &&
            (*out_method)->function != nullptr &&
            ((*out_method)->function->image_id == 0 ||
             (root_image != nullptr && (*out_method)->function->image_id == root_image->id));
        if (live_method_is_root && runtime->artifact_snapshot_index.has_value()) {
            const DartPlantStatus merge_status =
                dartplant::MergeValidatedArtifactIdentityIntoLiveMethod(
                    *runtime->artifact_snapshot_index, *query, *out_method);
            if (merge_status != DARTPLANT_OK) {
                dartplant_release_method(*out_method);
                *out_method = nullptr;
                status = merge_status;
            }
        }
    }
    if (status == DARTPLANT_METHOD_NOT_FOUND && runtime->artifact_snapshot_index.has_value()) {
        const auto* root_image = runtime->image_set.Root();
        status = dartplant::ResolveArtifactIndexedRuntimeMethod(
            *runtime->artifact_snapshot_index, *runtime->snapshot, *runtime->selected_app_module,
            root_image == nullptr ? 0 : root_image->id, runtime->entry_targets, *query,
            runtime->generation, runtime->generation->load(std::memory_order_acquire), out_method);
    }
    if (status == DARTPLANT_METHOD_NOT_FOUND && runtime->state != DARTPLANT_RUNTIME_READY) {
        dartplant::SetLastError(
            "method is not present in the exact artifact index and the live VM is not ready");
        status = DARTPLANT_RUNTIME_NOT_READY;
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
        runtime->diagnostics.function_candidate_count = 1;
        runtime->diagnostics.code_alias_count =
            (*out_method)->function != nullptr && (*out_method)->function->code_target != nullptr
                ? (*out_method)->function->code_target->KnownAliasCount()
                : 0;
        runtime->diagnostics.selected_entry = dartplant_method_runtime_address(*out_method);
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
