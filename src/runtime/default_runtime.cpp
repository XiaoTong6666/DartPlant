// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "runtime/default_runtime.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/internal.h"
#include "dartplant/advanced/artifact.h"
#include "dartplant/host_api.h"
#include "dartplant/runtime.h"
#include "runtime/runtime_internal.h"

namespace dartplant {
namespace {

struct DefaultRuntimeState {
    std::recursive_mutex mutex;
    DartPlantRuntime* runtime = nullptr;
    std::string app_module_name;
    std::string runtime_module_name;
    const HostApiBinding* host_binding = nullptr;
};

DefaultRuntimeState& DefaultRuntime() {
    static DefaultRuntimeState state;
    return state;
}

std::optional<std::string> CopyOptionalString(const char* value) {
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

const char* StringView(const std::optional<std::string>& value) {
    return value.has_value() ? value->c_str() : nullptr;
}

bool SameOptionalString(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) return left == right;
    return std::strcmp(left, right) == 0;
}

struct OwnedSnapshotFunction {
    std::optional<std::string> library_uri;
    std::optional<std::string> class_name;
    std::optional<std::string> function_name;
    std::optional<std::string> signature;
    std::optional<std::string> fingerprint;
    DartPlantSnapshotFunctionInfo view{};

    void RefreshView(const DartPlantSnapshotFunctionInfo& source) {
        view = source;
        view.struct_size = sizeof(view);
        view.library_uri = StringView(library_uri);
        view.class_name = StringView(class_name);
        view.function_name = StringView(function_name);
        view.signature = StringView(signature);
        view.fingerprint = StringView(fingerprint);
    }
};

struct OwnedSnapshotIndex {
    std::optional<std::string> module_name;
    std::optional<std::string> module_build_id;
    std::optional<std::string> snapshot_hash;
    std::optional<std::string> dart_version;
    std::optional<std::string> profile_version;
    std::vector<OwnedSnapshotFunction> functions;
    std::vector<DartPlantSnapshotFunctionInfo> function_views;
    DartPlantSnapshotIndexInfo view{};

    void RefreshView(const DartPlantSnapshotIndexInfo& source) {
        function_views.clear();
        function_views.reserve(functions.size());
        for (size_t index = 0; index < functions.size(); ++index) {
            functions[index].RefreshView(source.functions[index]);
            function_views.push_back(functions[index].view);
        }
        view = source;
        view.struct_size = sizeof(view);
        view.module_name = StringView(module_name);
        view.module_build_id = StringView(module_build_id);
        view.snapshot_hash = StringView(snapshot_hash);
        view.dart_version = StringView(dart_version);
        view.profile_version = StringView(profile_version);
        view.functions = function_views.empty() ? nullptr : function_views.data();
        view.function_count = static_cast<uint32_t>(function_views.size());
    }
};

struct OwnedCompilerEvidence {
    std::optional<std::string> snapshot_hash;
    std::optional<std::string> app_build_id;
    std::optional<std::string> code_fingerprint;
    std::vector<DartPlantAbiRepresentation> parameter_representations;
    std::optional<std::string> library_uri;
    std::optional<std::string> class_name;
    std::optional<std::string> function_name;
    DartPlantCompilerAbiEvidence view{};

    void RefreshView(const DartPlantCompilerAbiEvidence& source) {
        view = source;
        view.struct_size = sizeof(view);
        view.snapshot_hash = StringView(snapshot_hash);
        view.app_build_id = StringView(app_build_id);
        view.code_fingerprint = StringView(code_fingerprint);
        view.parameter_representations =
            parameter_representations.empty() ? nullptr : parameter_representations.data();
        view.parameter_count = static_cast<uint32_t>(parameter_representations.size());
        view.library_uri = StringView(library_uri);
        view.class_name = StringView(class_name);
        view.function_name = StringView(function_name);
    }
};

struct OwnedArtifactBundle {
    std::optional<OwnedSnapshotIndex> snapshot_index;
    std::vector<OwnedCompilerEvidence> compiler_evidence;
    std::vector<DartPlantCompilerAbiEvidence> compiler_evidence_views;
    DartPlantArtifactBundle view{};

    void RefreshView(const DartPlantArtifactBundle& source) {
        if (snapshot_index.has_value()) {
            snapshot_index->RefreshView(*source.snapshot_index);
        }
        compiler_evidence_views.clear();
        compiler_evidence_views.reserve(compiler_evidence.size());
        for (size_t index = 0; index < compiler_evidence.size(); ++index) {
            compiler_evidence[index].RefreshView(source.compiler_abi_evidence[index]);
            compiler_evidence_views.push_back(compiler_evidence[index].view);
        }
        view = source;
        view.struct_size = sizeof(view);
        view.version = DARTPLANT_ARTIFACT_BUNDLE_VERSION;
        view.snapshot_index = snapshot_index.has_value() ? &snapshot_index->view : nullptr;
        view.compiler_abi_evidence =
            compiler_evidence_views.empty() ? nullptr : compiler_evidence_views.data();
        view.compiler_abi_evidence_count = static_cast<uint32_t>(compiler_evidence_views.size());
    }
};

struct ArtifactRegistryState {
    std::mutex mutex;
    uint64_t generation = 0;
    uint64_t snapshot_generation = 0;
    std::vector<std::shared_ptr<const OwnedArtifactBundle>> bundles;
};

ArtifactRegistryState& ArtifactRegistry() {
    static ArtifactRegistryState state;
    return state;
}

bool ValidArtifactBundle(const DartPlantArtifactBundle* bundle) {
    if (bundle == nullptr) return true;
    if (bundle->struct_size < sizeof(DartPlantArtifactBundle) ||
        bundle->version != DARTPLANT_ARTIFACT_BUNDLE_VERSION ||
        (bundle->snapshot_index == nullptr && bundle->compiler_abi_evidence_count == 0) ||
        (bundle->compiler_abi_evidence_count != 0 && bundle->compiler_abi_evidence == nullptr)) {
        return false;
    }
    if (bundle->snapshot_index != nullptr &&
        bundle->snapshot_index->struct_size < sizeof(DartPlantSnapshotIndexInfo)) {
        return false;
    }
    if (bundle->snapshot_index != nullptr && bundle->snapshot_index->function_count != 0 &&
        bundle->snapshot_index->functions == nullptr) {
        return false;
    }
    if (bundle->snapshot_index != nullptr) {
        for (uint32_t index = 0; index < bundle->snapshot_index->function_count; ++index) {
            if (bundle->snapshot_index->functions[index].struct_size <
                sizeof(DartPlantSnapshotFunctionInfo)) {
                return false;
            }
        }
    }
    for (uint32_t index = 0; index < bundle->compiler_abi_evidence_count; ++index) {
        const auto& evidence = bundle->compiler_abi_evidence[index];
        if (evidence.struct_size < sizeof(DartPlantCompilerAbiEvidence) ||
            (evidence.parameter_count != 0 && evidence.parameter_representations == nullptr)) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<OwnedArtifactBundle> CopyArtifactBundle(const DartPlantArtifactBundle& source) {
    auto owned = std::make_shared<OwnedArtifactBundle>();
    if (source.snapshot_index != nullptr) {
        const auto& snapshot = *source.snapshot_index;
        owned->snapshot_index.emplace();
        auto& destination = *owned->snapshot_index;
        destination.module_name = CopyOptionalString(snapshot.module_name);
        destination.module_build_id = CopyOptionalString(snapshot.module_build_id);
        destination.snapshot_hash = CopyOptionalString(snapshot.snapshot_hash);
        destination.dart_version = CopyOptionalString(snapshot.dart_version);
        destination.profile_version = CopyOptionalString(snapshot.profile_version);
        destination.functions.reserve(snapshot.function_count);
        for (uint32_t index = 0; index < snapshot.function_count; ++index) {
            const auto& function = snapshot.functions[index];
            OwnedSnapshotFunction copy;
            copy.library_uri = CopyOptionalString(function.library_uri);
            copy.class_name = CopyOptionalString(function.class_name);
            copy.function_name = CopyOptionalString(function.function_name);
            copy.signature = CopyOptionalString(function.signature);
            copy.fingerprint = CopyOptionalString(function.fingerprint);
            destination.functions.push_back(std::move(copy));
        }
    }
    owned->compiler_evidence.reserve(source.compiler_abi_evidence_count);
    for (uint32_t index = 0; index < source.compiler_abi_evidence_count; ++index) {
        const auto& evidence = source.compiler_abi_evidence[index];
        OwnedCompilerEvidence copy;
        copy.snapshot_hash = CopyOptionalString(evidence.snapshot_hash);
        copy.app_build_id = CopyOptionalString(evidence.app_build_id);
        copy.code_fingerprint = CopyOptionalString(evidence.code_fingerprint);
        if (evidence.parameter_count != 0) {
            copy.parameter_representations.assign(
                evidence.parameter_representations,
                evidence.parameter_representations + evidence.parameter_count);
        }
        copy.library_uri = CopyOptionalString(evidence.library_uri);
        copy.class_name = CopyOptionalString(evidence.class_name);
        copy.function_name = CopyOptionalString(evidence.function_name);
        owned->compiler_evidence.push_back(std::move(copy));
    }
    owned->RefreshView(source);
    return owned;
}

bool SameSnapshotFunction(const DartPlantSnapshotFunctionInfo& left,
                          const DartPlantSnapshotFunctionInfo& right) {
    return SameOptionalString(left.library_uri, right.library_uri) &&
           SameOptionalString(left.class_name, right.class_name) &&
           SameOptionalString(left.function_name, right.function_name) &&
           SameOptionalString(left.signature, right.signature) &&
           left.entry_kind == right.entry_kind && left.entry_va == right.entry_va &&
           left.code_size == right.code_size && left.code_section_va == right.code_section_va &&
           SameOptionalString(left.fingerprint, right.fingerprint) &&
           left.code_identity_proof == right.code_identity_proof &&
           left.physical_entry_alias_count == right.physical_entry_alias_count;
}

bool SameSnapshotIndex(const DartPlantSnapshotIndexInfo* left,
                       const DartPlantSnapshotIndexInfo* right) {
    if (left == nullptr || right == nullptr) return left == right;
    if (!SameOptionalString(left->module_name, right->module_name) ||
        !SameOptionalString(left->module_build_id, right->module_build_id) ||
        !SameOptionalString(left->snapshot_hash, right->snapshot_hash) ||
        !SameOptionalString(left->dart_version, right->dart_version) ||
        !SameOptionalString(left->profile_version, right->profile_version) ||
        left->function_count != right->function_count) {
        return false;
    }
    for (uint32_t index = 0; index < left->function_count; ++index) {
        if (!SameSnapshotFunction(left->functions[index], right->functions[index])) return false;
    }
    return true;
}

bool SameCompilerEvidence(const DartPlantCompilerAbiEvidence& left,
                          const DartPlantCompilerAbiEvidence& right) {
    if (!SameOptionalString(left.snapshot_hash, right.snapshot_hash) ||
        !SameOptionalString(left.app_build_id, right.app_build_id) ||
        !SameOptionalString(left.code_fingerprint, right.code_fingerprint) ||
        left.parameter_count != right.parameter_count ||
        left.result_representation != right.result_representation ||
        left.max_parameters_in_registers != right.max_parameters_in_registers ||
        left.must_use_stack_calling_convention != right.must_use_stack_calling_convention ||
        left.has_optional_parameters != right.has_optional_parameters ||
        left.has_overrides_with_less_direct_parameters !=
            right.has_overrides_with_less_direct_parameters ||
        !SameOptionalString(left.library_uri, right.library_uri) ||
        !SameOptionalString(left.class_name, right.class_name) ||
        !SameOptionalString(left.function_name, right.function_name) ||
        left.entry_kind != right.entry_kind || left.entry_va != right.entry_va ||
        left.code_size != right.code_size) {
        return false;
    }
    for (uint32_t index = 0; index < left.parameter_count; ++index) {
        if (left.parameter_representations[index] != right.parameter_representations[index]) {
            return false;
        }
    }
    return true;
}

bool SameArtifactBundle(const DartPlantArtifactBundle& left, const DartPlantArtifactBundle& right) {
    if (left.version != right.version ||
        !SameSnapshotIndex(left.snapshot_index, right.snapshot_index) ||
        left.compiler_abi_evidence_count != right.compiler_abi_evidence_count) {
        return false;
    }
    for (uint32_t index = 0; index < left.compiler_abi_evidence_count; ++index) {
        if (!SameCompilerEvidence(left.compiler_abi_evidence[index],
                                  right.compiler_abi_evidence[index])) {
            return false;
        }
    }
    return true;
}

struct ArtifactRegistrySnapshot {
    uint64_t generation = 0;
    uint64_t snapshot_generation = 0;
    std::vector<std::shared_ptr<const OwnedArtifactBundle>> bundles;
};

ArtifactRegistrySnapshot SnapshotArtifactRegistry() {
    auto& registry = ArtifactRegistry();
    std::lock_guard lock(registry.mutex);
    return {
        .generation = registry.generation,
        .snapshot_generation = registry.snapshot_generation,
        .bundles = registry.bundles,
    };
}

DartPlantStatus BindArtifactIndexIfReady(DartPlantRuntime* runtime,
                                         const ArtifactRegistrySnapshot& registry) {
    if (runtime == nullptr || registry.bundles.empty() || registry.snapshot_generation == 0) {
        return DARTPLANT_OK;
    }

    std::string module_name;
    std::string module_build_id;
    std::string snapshot_hash;
    {
        std::lock_guard lock(runtime->mutex);
        if (runtime->bound_artifact_snapshot_generation == registry.snapshot_generation) {
            return DARTPLANT_OK;
        }
        if (!runtime->profile_matched || !runtime->selected_app_module.has_value() ||
            !runtime->snapshot.has_value()) {
            return DARTPLANT_OK;
        }
        module_name = runtime->selected_app_module->name;
        module_build_id = runtime->selected_app_module->build_id;
        snapshot_hash = runtime->snapshot->snapshot_hash;
    }

    const DartPlantSnapshotIndexInfo* first = nullptr;
    std::vector<DartPlantSnapshotFunctionInfo> functions;
    std::unordered_map<std::string, size_t> identities;
    for (const auto& owned_bundle : registry.bundles) {
        const auto* bundle = &owned_bundle->view;
        if (bundle == nullptr || bundle->snapshot_index == nullptr) continue;
        const auto& source = *bundle->snapshot_index;
        const std::string_view source_module =
            source.module_name == nullptr ? "" : source.module_name;
        const std::string_view source_build =
            source.module_build_id == nullptr ? "" : source.module_build_id;
        const std::string_view source_snapshot =
            source.snapshot_hash == nullptr ? "" : source.snapshot_hash;
        if (source_module != module_name || source_build.empty() ||
            !EqualsIgnoreCaseAscii(module_build_id, std::string(source_build)) ||
            source_snapshot != snapshot_hash) {
            // Embedded bundles are optional and can coexist in a process.
            // Ignore artifacts for another app incarnation instead of making
            // retained live-VM method lookup fail.
            continue;
        }
        if (source.functions == nullptr || source.function_count == 0) {
            SetLastError("matching embedded artifact index has no Function records");
            return DARTPLANT_METADATA_INVALID;
        }
        if (first == nullptr) first = &source;
        for (uint32_t index = 0; index < source.function_count; ++index) {
            const auto& function = source.functions[index];
            if (function.struct_size < sizeof(DartPlantSnapshotFunctionInfo) ||
                function.library_uri == nullptr || function.function_name == nullptr) {
                SetLastError("matching embedded artifact contains an invalid Function record");
                return DARTPLANT_METADATA_INVALID;
            }
            const std::string key = std::string(function.library_uri) + '\n' +
                                    (function.class_name == nullptr ? "" : function.class_name) +
                                    '\n' + function.function_name + '\n' +
                                    (function.signature == nullptr ? "" : function.signature) +
                                    '\n' +
                                    std::to_string(static_cast<uint32_t>(function.entry_kind));
            const auto [position, inserted] = identities.emplace(key, functions.size());
            if (inserted) {
                functions.push_back(function);
                continue;
            }
            const auto& current = functions[position->second];
            const std::string_view current_fingerprint =
                current.fingerprint == nullptr ? "" : current.fingerprint;
            const std::string_view new_fingerprint =
                function.fingerprint == nullptr ? "" : function.fingerprint;
            if (current.entry_va != function.entry_va || current.code_size != function.code_size ||
                current.code_section_va != function.code_section_va ||
                current_fingerprint != new_fingerprint ||
                current.code_identity_proof != function.code_identity_proof ||
                current.physical_entry_alias_count != function.physical_entry_alias_count) {
                SetLastError(
                    "embedded artifact bundles disagree about one logical Function identity");
                return DARTPLANT_METADATA_INVALID;
            }
        }
    }
    if (first == nullptr) {
        std::lock_guard lock(runtime->mutex);
        runtime->bound_artifact_snapshot_generation = registry.snapshot_generation;
        return DARTPLANT_OK;
    }

    DartPlantSnapshotIndexInfo merged = *first;
    merged.functions = functions.data();
    merged.function_count = static_cast<uint32_t>(functions.size());
    const DartPlantStatus status =
        ReplaceRuntimeArtifactSnapshotIndex(runtime, &merged, registry.snapshot_generation);
    return status;
}

DartPlantStatus EnsureDefaultRuntimeImagesReady(DefaultRuntimeState& state) {
    if (state.runtime == nullptr) {
        SetLastError("DartPlant is not initialized");
        return DARTPLANT_NOT_INITIALIZED;
    }

    // Normal consumers do not need to forward dlopen events manually. Refresh
    // the complete image set before the first/next lookup; host adapters may
    // additionally keep the background refresh worker informed.
    const DartPlantStatus refresh_status =
        dartplant_runtime_on_module_loaded(state.runtime, nullptr, nullptr);
    if (refresh_status != DARTPLANT_OK && refresh_status != DARTPLANT_RUNTIME_NOT_READY) {
        return refresh_status;
    }

    DartPlantStatus artifact_status = BindRegisteredArtifactIndexIfReady(state.runtime);
    if (artifact_status != DARTPLANT_OK) return artifact_status;

    DartPlantRuntimeInfo info{};
    info.struct_size = sizeof(info);
    DartPlantStatus status = dartplant_runtime_get_info(state.runtime, &info);
    if (status != DARTPLANT_OK) return status;
    if (info.state == DARTPLANT_RUNTIME_READY || info.state == DARTPLANT_RUNTIME_IMAGES_READY) {
        return DARTPLANT_OK;
    }
    SetLastError("Flutter app/runtime images are not ready for Dart method lookup");
    return DARTPLANT_RUNTIME_NOT_READY;
}

bool EvidenceMatchesMethod(const DartPlantCompilerAbiEvidence& evidence,
                           const DartPlantMethod* method) {
    if (method == nullptr || method->function == nullptr ||
        evidence.struct_size < sizeof(DartPlantCompilerAbiEvidence)) {
        return false;
    }
    const auto& identity = method->function->identity;
    const std::string_view library = evidence.library_uri == nullptr ? "" : evidence.library_uri;
    const std::string_view class_name = evidence.class_name == nullptr ? "" : evidence.class_name;
    const std::string_view function =
        evidence.function_name == nullptr ? "" : evidence.function_name;
    return library == identity.library_uri && class_name == identity.class_name &&
           function == identity.function_name && evidence.entry_kind == identity.entry_kind;
}

bool EvidenceMatchesCurrentArtifact(DartPlantRuntime* runtime,
                                    const DartPlantCompilerAbiEvidence& evidence,
                                    const DartPlantMethod* method) {
    if (!EvidenceMatchesMethod(evidence, method) || evidence.snapshot_hash == nullptr ||
        evidence.app_build_id == nullptr || evidence.code_fingerprint == nullptr ||
        evidence.entry_va == 0 || evidence.code_size == 0) {
        return false;
    }
    std::lock_guard lock(runtime->mutex);
    if (!runtime->snapshot.has_value() || !runtime->selected_app_module.has_value()) return false;
    if (runtime->snapshot->snapshot_hash != evidence.snapshot_hash ||
        !EqualsIgnoreCaseAscii(runtime->selected_app_module->build_id, evidence.app_build_id) ||
        evidence.code_size != MethodCodeSize(method)) {
        return false;
    }
    const auto target =
        runtime->snapshot->ResolveInstructionVa(*runtime->selected_app_module, evidence.entry_va);
    if (!target.has_value() || *target != MethodTarget(method)) return false;
    if (!method->record.fingerprint.empty() &&
        method->record.fingerprint != evidence.code_fingerprint) {
        return false;
    }
    return true;
}

DartPlantStatus BindCompilerEvidenceIfPresent(DartPlantRuntime* runtime,
                                              const ArtifactRegistrySnapshot& registry,
                                              const DartPlantMethod* method) {
    if (registry.bundles.empty() || method == nullptr) {
        return DARTPLANT_OK;
    }

    DartPlantMethodAbiInfo info{};
    info.struct_size = sizeof(info);
    const DartPlantStatus info_status =
        dartplant_runtime_get_method_abi_info(runtime, method, &info);
    if (info_status != DARTPLANT_OK) return info_status;
    // Evidence is immutable for a runtime generation. Do not append an
    // identical provider every time the same logical method is resolved.
    if (info.state != DARTPLANT_METHOD_ABI_NONE) return DARTPLANT_OK;

    bool matched = false;
    for (const auto& owned_bundle : registry.bundles) {
        const auto* bundle = &owned_bundle->view;
        if (bundle == nullptr || bundle->compiler_abi_evidence == nullptr) continue;
        for (uint32_t index = 0; index < bundle->compiler_abi_evidence_count; ++index) {
            const auto& evidence = bundle->compiler_abi_evidence[index];
            // Logical identity is only the first filter. A process can hold
            // sidecars for several app incarnations with the same Dart name;
            // stale snapshot/build/entry/fingerprint evidence must be skipped
            // without preventing a later exact candidate from binding.
            if (!EvidenceMatchesCurrentArtifact(runtime, evidence, method)) continue;
            matched = true;
            const DartPlantStatus status =
                dartplant_runtime_register_compiler_abi_evidence(runtime, method, &evidence);
            if (status == DARTPLANT_OK) {
                ClearLastError();
                return DARTPLANT_OK;
            }
            if (status == DARTPLANT_RUNTIME_NOT_READY) return status;
            // Compiler ABI evidence is an optional typed overlay. A stale,
            // unsupported or conflicting provider must never prevent the
            // already-resolved method from remaining usable through raw
            // invocation APIs.
            ClearLastError();
        }
    }
    if (matched) ClearLastError();
    return DARTPLANT_OK;
}

}  // namespace

bool DefaultRuntimeInitialized() {
    auto& state = DefaultRuntime();
    std::lock_guard lock(state.mutex);
    return state.runtime != nullptr;
}

DartPlantRuntime* DefaultRuntimeInstanceForTesting() {
    auto& state = DefaultRuntime();
    std::lock_guard lock(state.mutex);
    return state.runtime;
}

DartPlantStatus BindRegisteredArtifactIndexIfReady(DartPlantRuntime* runtime) {
    return BindArtifactIndexIfReady(runtime, SnapshotArtifactRegistry());
}

DartPlantStatus BindRegisteredCompilerEvidenceIfPresent(DartPlantRuntime* runtime,
                                                        const DartPlantMethod* method) {
    return BindCompilerEvidenceIfPresent(runtime, SnapshotArtifactRegistry(), method);
}

DartPlantStatus FindDefaultRuntimeMethod(const DartPlantMethodQuery* query,
                                         DartPlantMethod** out_method) {
    auto& state = DefaultRuntime();
    std::lock_guard lock(state.mutex);
    DartPlantStatus status = EnsureDefaultRuntimeImagesReady(state);
    if (status != DARTPLANT_OK) return status;
    status = dartplant_runtime_find_method(state.runtime, query, out_method);
    if (status == DARTPLANT_RUNTIME_NOT_READY) {
        // Exact artifact methods (including PRODUCT-dropped Functions) resolve
        // directly from IMAGES_READY. Only a miss needs semantic VM roots and
        // the live Function index, so ordinary artifact consumers never depend
        // on sampler timing.
        status = dartplant_runtime_bootstrap_live_vm(state.runtime, nullptr, nullptr);
        if (status != DARTPLANT_OK) return status;
        status = BindRegisteredArtifactIndexIfReady(state.runtime);
        if (status != DARTPLANT_OK) return status;
        status = dartplant_runtime_find_method(state.runtime, query, out_method);
    }
    if (status != DARTPLANT_OK) return status;
    ClearLastError();
    return DARTPLANT_OK;
}

}  // namespace dartplant

extern "C" {

DartPlantStatus dartplant_init(const DartPlantInitInfo* info) {
    if (info == nullptr || info->struct_size < sizeof(DartPlantInitInfo) ||
        info->version != DARTPLANT_INIT_API_VERSION ||
        !dartplant::ValidArtifactBundle(info->artifact_bundle) ||
        (info->host_api != nullptr &&
         (info->host_api->struct_size < sizeof(DartPlantHostApi) ||
          info->host_api->version < DARTPLANT_HOST_API_VERSION || info->host_api->hook == nullptr ||
          info->host_api->unhook == nullptr))) {
        dartplant::SetLastError("DartPlant init info is invalid or unsupported");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    auto& state = dartplant::DefaultRuntime();
    std::lock_guard lock(state.mutex);
    DartPlantRuntimeProfile requested_profile{};
    dartplant_runtime_profile_init_arm64_aot(&requested_profile);
    if (info->app_module_name != nullptr && info->app_module_name[0] != '\0') {
        requested_profile.app_module_name = info->app_module_name;
    }
    if (info->runtime_module_name != nullptr && info->runtime_module_name[0] != '\0') {
        requested_profile.runtime_module_name = info->runtime_module_name;
    }

    if (state.runtime != nullptr) {
        const auto* binding = state.host_binding;
        const bool host_matches =
            info->host_api == nullptr ||
            (binding != nullptr && info->host_api->user_data == binding->user_data &&
             info->host_api->hook == binding->hook && info->host_api->unhook == binding->unhook);
        if (!host_matches || state.app_module_name != requested_profile.app_module_name ||
            state.runtime_module_name != requested_profile.runtime_module_name) {
            dartplant::SetLastError(
                "DartPlant is already initialized with a different host or module configuration");
            return DARTPLANT_PROFILE_MISMATCH;
        }
        if (info->artifact_bundle != nullptr) {
            const DartPlantStatus artifact_status =
                dartplant_register_embedded_artifact_bundle(info->artifact_bundle);
            if (artifact_status != DARTPLANT_OK) return artifact_status;
            return dartplant::BindRegisteredArtifactIndexIfReady(state.runtime);
        }
        dartplant::ClearLastError();
        return DARTPLANT_OK;
    }

    if (info->host_api != nullptr) {
        const DartPlantStatus host_status = dartplant_install_host_api(info->host_api);
        if (host_status != DARTPLANT_OK) return host_status;
    }
    const auto* host_binding = dartplant::State().host.binding.load(std::memory_order_acquire);
    if (host_binding == nullptr) {
        dartplant::SetLastError("DartPlant init requires a host hook backend");
        return DARTPLANT_HOST_API_UNAVAILABLE;
    }

    if (info->artifact_bundle != nullptr) {
        const DartPlantStatus artifact_status =
            dartplant_register_embedded_artifact_bundle(info->artifact_bundle);
        if (artifact_status != DARTPLANT_OK) return artifact_status;
    }

    DartPlantRuntime* runtime = nullptr;
    const DartPlantStatus status = dartplant_runtime_create(&requested_profile, &runtime);
    if (status != DARTPLANT_OK) return status;
    state.runtime = runtime;
    state.app_module_name = requested_profile.app_module_name;
    state.runtime_module_name = requested_profile.runtime_module_name;
    state.host_binding = host_binding;

    dartplant::StartRuntimeModuleRefreshWorker(nullptr);
    dartplant::ScheduleRuntimeModuleRefresh();
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

void dartplant_shutdown(void) {
    auto& state = dartplant::DefaultRuntime();
    DartPlantRuntime* runtime = nullptr;
    const dartplant::HostApiBinding* host_binding = nullptr;
    {
        std::lock_guard lock(state.mutex);
        runtime = state.runtime;
        host_binding = state.host_binding;
        // Clearing the current process binding is part of the same state
        // transition as publishing runtime == nullptr. Otherwise a concurrent
        // init(host_api=nullptr) could inherit this borrowed binding after the
        // old runtime has been detached but before shutdown clears it.
        if (runtime != nullptr) dartplant::ClearHostApi(host_binding);
        state.runtime = nullptr;
        state.app_module_name.clear();
        state.runtime_module_name.clear();
        state.host_binding = nullptr;
    }
    if (runtime != nullptr) dartplant_runtime_destroy(runtime);
}

uint8_t dartplant_is_initialized(void) { return dartplant::DefaultRuntimeInitialized() ? 1 : 0; }

DartPlantStatus dartplant_register_embedded_artifact_bundle(const DartPlantArtifactBundle* bundle) {
    if (!dartplant::ValidArtifactBundle(bundle) || bundle == nullptr) {
        dartplant::SetLastError("embedded DartPlant artifact bundle is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto owned = dartplant::CopyArtifactBundle(*bundle);
    if (owned == nullptr) {
        dartplant::SetLastError("failed to copy embedded DartPlant artifact bundle");
        return DARTPLANT_METADATA_INVALID;
    }
    auto& registry = dartplant::ArtifactRegistry();
    std::lock_guard lock(registry.mutex);
    const auto duplicate = std::find_if(
        registry.bundles.begin(), registry.bundles.end(), [&owned](const auto& existing) {
            return dartplant::SameArtifactBundle(existing->view, owned->view);
        });
    if (duplicate == registry.bundles.end()) {
        registry.bundles.push_back(std::move(owned));
        ++registry.generation;
        if (bundle->snapshot_index != nullptr) ++registry.snapshot_generation;
    }
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_hook_method(const DartPlantMethod* method,
                                      const DartPlantHookOptions* options,
                                      DartPlantHookHandle** out_handle) {
    if (method == nullptr || options == nullptr || out_handle == nullptr) {
        dartplant::SetLastError("logical method hook arguments are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto& state = dartplant::DefaultRuntime();
    std::lock_guard lock(state.mutex);
    if (state.runtime == nullptr) {
        dartplant::SetLastError("DartPlant is not initialized");
        return DARTPLANT_NOT_INITIALIZED;
    }
    return dartplant_runtime_hook_method_handle(state.runtime, method, options, out_handle);
}

DartPlantStatus dartplant_unhook_handle(DartPlantHookHandle* handle) {
    if (handle == nullptr || handle->listener == nullptr) {
        dartplant::SetLastError("logical hook handle is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    if (handle->removed) return DARTPLANT_OK;
    const DartPlantStatus status = dartplant_remove_listener(handle->listener);
    if (status == DARTPLANT_OK) handle->removed = true;
    return status;
}

uint8_t dartplant_hook_handle_is_active(const DartPlantHookHandle* handle) {
    return handle != nullptr && handle->listener != nullptr && !handle->removed
               ? dartplant_listener_is_active(handle->listener)
               : 0;
}

uint8_t dartplant_hook_handle_is_idle(const DartPlantHookHandle* handle) {
    return handle != nullptr && handle->listener != nullptr
               ? dartplant_listener_is_idle(handle->listener)
               : 1;
}

void dartplant_release_hook_handle(DartPlantHookHandle* handle) {
    if (handle == nullptr) return;
    if (handle->listener != nullptr && !dartplant_listener_is_idle(handle->listener)) {
        dartplant::SetLastError("logical hook handle still has an in-flight invocation");
        return;
    }
    if (!handle->removed && handle->listener != nullptr) {
        const DartPlantStatus status = dartplant_remove_listener(handle->listener);
        if (status != DARTPLANT_OK) return;
        handle->removed = true;
    }
    dartplant_release_listener(handle->listener);
    handle->listener = nullptr;
    delete handle;
}

}  // extern "C"
