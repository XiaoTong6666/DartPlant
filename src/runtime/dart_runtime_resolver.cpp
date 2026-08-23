// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "runtime/dart_vm_abi.h"
#include "runtime/runtime_internal.h"

namespace dartplant {
namespace {

bool BuildIdMatches(const ModuleImage& module, const std::string& expected) {
    return expected.empty() || EqualsIgnoreCaseAscii(module.build_id, expected);
}

std::optional<ModuleImage> ProfileModule(const std::vector<ModuleImage>& modules,
                                         const std::string& name, const std::string& build_id) {
    const auto module = FindModule(modules, name);
    if (!module.has_value() || !BuildIdMatches(*module, build_id)) {
        return std::nullopt;
    }
    return module;
}

void FillSnapshotInfo(const FlutterSnapshotSource& snapshot, DartPlantFlutterSnapshotInfo* info) {
    info->module_name = snapshot.module_name.c_str();
    info->module_path = snapshot.module_path.c_str();
    info->module_build_id = snapshot.module_build_id.c_str();
    info->snapshot_hash = snapshot.snapshot_hash.c_str();
    info->snapshot_features = snapshot.snapshot_features.c_str();
    info->profile_name = snapshot.profile_name.c_str();
    info->load_bias = snapshot.isolate_instructions_runtime - snapshot.isolate_instructions_va;
    info->isolate_instructions_va = snapshot.isolate_instructions_va;
    info->isolate_instructions_size = snapshot.isolate_instructions_size;
    info->isolate_instructions_runtime = snapshot.isolate_instructions_runtime;
    info->compressed_pointers = snapshot.compressed_pointers ? 1 : 0;
}

bool CanUseLiveVmForQuery(const DartPlantMethodQuery& query) {
    return query.entry_kind == DARTPLANT_ENTRY_DEFAULT && query.class_name != nullptr &&
           query.class_name[0] != '\0' &&
           (query.signature == nullptr || query.signature[0] == '\0');
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
           left->compressed_pointers == right->compressed_pointers;
}

bool SameSemanticContext(const DartPlantLiveVmContext& left, uint64_t left_null,
                         const DartPlantLiveVmContext& right, uint64_t right_null) {
    return left.profile_version == right.profile_version && left.isolate == right.isolate &&
           left.isolate_group == right.isolate_group && left.class_table == right.class_table &&
           left.cached_class_table_table == right.cached_class_table_table &&
           left.object_store == right.object_store && left.heap_base == right.heap_base &&
           left.pp == right.pp && left.global_object_pool == right.global_object_pool &&
           left.object_pool_length == right.object_pool_length && left_null == right_null;
}

std::mutex& RuntimeRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<DartPlantRuntime*>& RuntimeRegistry() {
    static std::vector<DartPlantRuntime*> runtimes;
    return runtimes;
}

void RegisterRuntime(DartPlantRuntime* runtime) {
    std::lock_guard lock(RuntimeRegistryMutex());
    RuntimeRegistry().push_back(runtime);
}

void UnregisterRuntime(DartPlantRuntime* runtime) {
    std::lock_guard lock(RuntimeRegistryMutex());
    std::erase(RuntimeRegistry(), runtime);
}

DartPlantStatus ResolveLiveIndexedRuntimeMethod(const SnapshotIndex& index,
                                                const std::vector<ModuleImage>& modules,
                                                DartCodeTargetRegistry& code_targets,
                                                const DartPlantMethodQuery& query,
                                                DartPlantMethod** out_method) {
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
    const auto module = FindModule(modules, index.module_name);
    if (!module.has_value() ||
        !module->ContainsExecutable(record->runtime_entry, record->code_size)) {
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

    auto code_target =
        code_targets.GetOrCreate(record->runtime_entry, static_cast<uint32_t>(record->code_size),
                                 record->code_object, record->entry_alias_count);
    if (code_target == nullptr) {
        SetLastError("live Function index produced an invalid CodeTarget");
        return DARTPLANT_METHOD_NOT_FOUND;
    }
    auto function = std::make_shared<DartFunctionHandle>();
    function->identity = MethodIdentityFromRecord(method_record);
    function->function_object = record->function_object;
    function->code_object = record->code_object;
    function->source = DartFunctionSource::kLiveVm;
    function->code_target = code_target;
    code_target->AddAlias(function->identity);

    auto* method = new DartPlantMethod;
    method->record = std::move(method_record);
    method->module = *module;
    method->function = std::move(function);
    *out_method = method;
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

DartPlantStatus RefreshRuntimeModules(DartPlantRuntime* runtime,
                                      const std::vector<ModuleImage>& modules) {
    if (runtime == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    std::lock_guard lock(runtime->mutex);

    const auto old_app = FindModule(runtime->modules, runtime->profile.app_module_name);
    const auto old_dart_runtime =
        FindModule(runtime->modules, runtime->profile.runtime_module_name);
    const auto new_app = FindModule(modules, runtime->profile.app_module_name);
    const auto new_dart_runtime = FindModule(modules, runtime->profile.runtime_module_name);
    const bool app_changed = !SameModuleIdentity(old_app, new_app);
    const bool dart_runtime_changed = !SameModuleIdentity(old_dart_runtime, new_dart_runtime);

    std::optional<FlutterSnapshotSource> snapshot;
    if (!app_changed && runtime->snapshot.has_value()) {
        snapshot = runtime->snapshot;
    } else if (new_app.has_value()) {
        std::string snapshot_error;
        snapshot = DiscoverFlutterSnapshot(*new_app, &snapshot_error);
    }
    const bool snapshot_changed = !SameSnapshotIdentity(runtime->snapshot, snapshot);
    const bool relevant_identity_changed = app_changed || dart_runtime_changed || snapshot_changed;

    runtime->modules = modules;
    if (relevant_identity_changed) {
        runtime->generation->fetch_add(1, std::memory_order_acq_rel);
        InvalidateRuntimeHooks(runtime->generation);
        runtime->code_targets.Clear();
        runtime->snapshot = std::move(snapshot);
        runtime->live_vm_context.reset();
        runtime->live_vm_null_value = 0;
        runtime->live_snapshot_index.reset();
        runtime->live_function_index_info = {};
    }

    const auto app = ProfileModule(runtime->modules, runtime->profile.app_module_name,
                                   runtime->profile.app_build_id);
    const auto dart_runtime = ProfileModule(runtime->modules, runtime->profile.runtime_module_name,
                                            runtime->profile.runtime_build_id);
    runtime->profile_matched = app.has_value() && dart_runtime.has_value();
    if (!runtime->profile_matched) {
        runtime->state = DARTPLANT_RUNTIME_CREATED;
        SetLastError("runtime profile modules are not loaded or mismatch");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!runtime->snapshot.has_value()) {
        runtime->state = DARTPLANT_RUNTIME_FAILED;
        SetLastError("loaded app module has no usable Flutter snapshot source");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (relevant_identity_changed || runtime->state != DARTPLANT_RUNTIME_READY) {
        runtime->state = DARTPLANT_RUNTIME_IMAGES_READY;
    }
    ClearLastError();
    return DARTPLANT_OK;
}

void NotifyRuntimeModuleLoaded(const char*, void*) {
    const auto modules = EnumerateModules();
    std::lock_guard lock(RuntimeRegistryMutex());
    for (DartPlantRuntime* runtime : RuntimeRegistry()) {
        RefreshRuntimeModules(runtime, modules);
    }
    ClearLastError();
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
    DartPlantLiveVmFunctionIndexInfo index_info{};
    index_info.struct_size = sizeof(index_info);
    std::string error;
    auto index = BuildLiveSnapshotIndex(context, snapshot_info, &index_info, &error);
    if (!index.has_value()) {
        SetLastError(error.empty() ? "failed to build live Function index" : error);
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (runtime->live_vm_context.has_value() &&
        !SameSemanticContext(*runtime->live_vm_context, runtime->live_vm_null_value, context,
                             validated_null_value)) {
        runtime->generation->fetch_add(1, std::memory_order_acq_rel);
        InvalidateRuntimeHooks(runtime->generation);
        runtime->code_targets.Clear();
    }
    runtime->live_vm_context = context;
    runtime->live_vm_null_value = validated_null_value;
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
    dartplant::UnregisterRuntime(runtime);
    runtime->generation->fetch_add(1, std::memory_order_acq_rel);
    dartplant::InvalidateRuntimeHooks(runtime->generation);
    delete runtime;
}

DartPlantStatus dartplant_runtime_on_module_loaded(DartPlantRuntime* runtime, const char*, void*) {
    if (runtime == nullptr) {
        dartplant::SetLastError("runtime is null");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto modules = dartplant::EnumerateModules();
    return dartplant::RefreshRuntimeModules(runtime, modules);
}

DartPlantStatus dartplant_runtime_get_info(const DartPlantRuntime* runtime,
                                           DartPlantRuntimeInfo* out_info) {
    if (runtime == nullptr || out_info == nullptr ||
        out_info->struct_size < sizeof(DartPlantRuntimeInfo)) {
        dartplant::SetLastError("runtime info arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(runtime->mutex);
    out_info->state = runtime->state;
    out_info->loaded_module_count = static_cast<uint32_t>(runtime->modules.size());
    out_info->app_module_loaded =
        dartplant::FindModule(runtime->modules, runtime->profile.app_module_name).has_value() ? 1
                                                                                              : 0;
    out_info->runtime_module_loaded =
        dartplant::FindModule(runtime->modules, runtime->profile.runtime_module_name).has_value()
            ? 1
            : 0;
    out_info->live_function_index_ready = runtime->live_snapshot_index.has_value() ? 1 : 0;
    out_info->profile_matched = runtime->profile_matched ? 1 : 0;
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_runtime_get_flutter_snapshot(const DartPlantRuntime* runtime,
                                                       DartPlantFlutterSnapshotInfo* out_info) {
    if (runtime == nullptr || out_info == nullptr || out_info->struct_size < sizeof(*out_info)) {
        dartplant::SetLastError("runtime snapshot info arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
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
    std::lock_guard lock(runtime->mutex);
    if (!runtime->snapshot.has_value()) {
        dartplant::SetLastError("Flutter snapshot source is not available for live VM capture");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    dartplant::FillSnapshotInfo(*runtime->snapshot, &snapshot_info);
    DartPlantLiveVmProbeInfo probe{};
    probe.struct_size = sizeof(probe);
    DartPlantStatus status = dartplant_live_vm_probe_invocation(invocation, &snapshot_info, &probe);
    if (status != DARTPLANT_OK) return status;

    DartPlantLiveVmProfile profile{};
    profile.struct_size = sizeof(profile);
    status = dartplant_live_vm_select_profile(&snapshot_info, &profile);
    if (status != DARTPLANT_OK) return status;

    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    status = dartplant_live_vm_context_from_probe(&probe, &context);
    if (status != DARTPLANT_OK) return status;
    status = dartplant::BuildLiveIndexForContext(runtime, context, *runtime->snapshot,
                                                 invocation->context->x[profile.null_register]);
    if (status != DARTPLANT_OK) return status;

    if (invocation->requested_method != nullptr &&
        invocation->requested_method->function != nullptr &&
        invocation->requested_method->function->code_target != nullptr) {
        auto& function = *invocation->requested_method->function;
        function.function_object = probe.function;
        function.code_object = probe.code;
        function.code_target->Update(dartplant::MethodCodeSize(invocation->requested_method),
                                     probe.code, probe.entry_alias_count);
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
            return DARTPLANT_SHARED_CODE_ENTRY;
        }
    }

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

    dartplant::FlutterSnapshotSource snapshot;
    {
        std::lock_guard lock(runtime->mutex);
        if (!runtime->profile_matched || !runtime->snapshot.has_value()) {
            dartplant::SetLastError("runtime images/snapshot are not ready for cold bootstrap");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        snapshot = *runtime->snapshot;
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
    if (status != DARTPLANT_OK) return status;

    {
        std::lock_guard lock(runtime->mutex);
        if (!runtime->snapshot.has_value() ||
            runtime->snapshot->snapshot_hash != snapshot.snapshot_hash ||
            runtime->snapshot->module_build_id != snapshot.module_build_id) {
            dartplant::SetLastError("runtime snapshot changed during cold bootstrap");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        const DartPlantStatus index_status = dartplant::BuildLiveIndexForContext(
            runtime, context, snapshot, bootstrap_info.last_candidate_null);
        if (index_status != DARTPLANT_OK) return index_status;
    }
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

    dartplant::FlutterSnapshotSource snapshot;
    {
        std::lock_guard lock(runtime->mutex);
        if (!runtime->profile_matched || !runtime->snapshot.has_value()) {
            dartplant::SetLastError("runtime images/snapshot are not ready for register bootstrap");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        snapshot = *runtime->snapshot;
    }

    DartPlantFlutterSnapshotInfo snapshot_info{};
    snapshot_info.struct_size = sizeof(snapshot_info);
    dartplant::FillSnapshotInfo(snapshot, &snapshot_info);
    DartPlantLiveVmContext context{};
    context.struct_size = sizeof(context);
    const DartPlantStatus status =
        dartplant_live_vm_context_from_arm64_registers(&snapshot_info, registers, &context);
    if (status != DARTPLANT_OK) return status;

    {
        std::lock_guard lock(runtime->mutex);
        if (!runtime->snapshot.has_value() ||
            runtime->snapshot->snapshot_hash != snapshot.snapshot_hash ||
            runtime->snapshot->module_build_id != snapshot.module_build_id) {
            dartplant::SetLastError("runtime snapshot changed during register bootstrap");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
        const DartPlantStatus index_status =
            dartplant::BuildLiveIndexForContext(runtime, context, snapshot, registers->null_value);
        if (index_status != DARTPLANT_OK) return index_status;
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
    if (runtime == nullptr || out_info == nullptr ||
        out_info->struct_size < sizeof(DartPlantLiveVmFunctionInfo)) {
        dartplant::SetLastError("runtime Function info arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    std::lock_guard lock(runtime->mutex);
    if (!runtime->live_snapshot_index.has_value() ||
        index >= runtime->live_snapshot_index->functions.size()) {
        dartplant::SetLastError("runtime Function index position is out of range");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    const auto& source = runtime->live_snapshot_index->functions[index];
    DartPlantLiveVmFunctionInfo info{};
    info.struct_size = sizeof(info);
    info.entry_alias_count = source.entry_alias_count;
    info.function = source.function_object;
    info.code = source.code_object;
    info.code_object_pool = source.code_object_pool;
    info.function_entry_point = source.runtime_entry;
    info.code_entry_point = source.code_entry;
    info.entry_va = source.entry_va;
    info.code_section_va = source.code_section_va;
    info.code_size = static_cast<uint32_t>(source.code_size);
    info.function_kind = source.function_kind;
    info.owner_class = source.owner_class;
    info.library = source.library;
    info.owner_is_toplevel_class = source.owner_is_toplevel_class ? 1 : 0;
    info.entry_is_shared = source.entry_alias_count > 1 ? 1 : 0;
    info.code_owner_matches_function = source.code_owner_matches_function ? 1 : 0;
    std::snprintf(info.library_uri, sizeof(info.library_uri), "%s", source.library_uri.c_str());
    std::snprintf(info.class_name, sizeof(info.class_name), "%s", source.class_name.c_str());
    std::snprintf(info.function_name, sizeof(info.function_name), "%s",
                  source.function_name.c_str());
    *out_info = info;
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
    std::lock_guard lock(runtime->mutex);
    if (runtime->state != DARTPLANT_RUNTIME_READY) {
        dartplant::SetLastError("runtime is not ready for method resolution");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!runtime->live_vm_context.has_value() || !runtime->snapshot.has_value() ||
        !runtime->live_snapshot_index.has_value()) {
        dartplant::SetLastError("runtime live Function index is not available");
        return DARTPLANT_RUNTIME_NOT_READY;
    }
    if (!dartplant::CanUseLiveVmForQuery(*query)) {
        dartplant::SetLastError(
            "method query is outside the supported live Function-index identity domain");
        return DARTPLANT_METHOD_NOT_FOUND;
    }
    return dartplant::ResolveLiveIndexedRuntimeMethod(
        *runtime->live_snapshot_index, runtime->modules, runtime->code_targets, *query, out_method);
}

}  // extern "C"
