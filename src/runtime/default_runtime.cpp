// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "runtime/default_runtime.h"

#include <algorithm>
#include <cstring>
#include <mutex>
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
};

DefaultRuntimeState& DefaultRuntime() {
    static DefaultRuntimeState state;
    return state;
}

struct ArtifactRegistryState {
    std::mutex mutex;
    std::vector<const DartPlantArtifactBundle*> bundles;
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
    for (uint32_t index = 0; index < bundle->compiler_abi_evidence_count; ++index) {
        if (bundle->compiler_abi_evidence[index].struct_size <
            sizeof(DartPlantCompilerAbiEvidence)) {
            return false;
        }
    }
    return true;
}

bool ArtifactIndexAlreadyBound(DartPlantRuntime* runtime) {
    if (runtime == nullptr) return false;
    std::lock_guard lock(runtime->mutex);
    return runtime->artifact_snapshot_index.has_value();
}

DartPlantStatus BindArtifactIndexIfReady(
    DartPlantRuntime* runtime, const std::vector<const DartPlantArtifactBundle*>& bundles) {
    if (runtime == nullptr || bundles.empty() || ArtifactIndexAlreadyBound(runtime)) {
        return DARTPLANT_OK;
    }

    std::string module_name;
    std::string module_build_id;
    std::string snapshot_hash;
    {
        std::lock_guard lock(runtime->mutex);
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
    for (const auto* bundle : bundles) {
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
        return DARTPLANT_OK;
    }

    DartPlantSnapshotIndexInfo merged = *first;
    merged.functions = functions.data();
    merged.function_count = static_cast<uint32_t>(functions.size());
    return dartplant_runtime_register_snapshot_index(runtime, &merged);
}

DartPlantStatus EnsureDefaultRuntimeReady(DefaultRuntimeState& state) {
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
    if (info.state == DARTPLANT_RUNTIME_READY && info.live_function_index_ready != 0) {
        return DARTPLANT_OK;
    }
    if (info.state != DARTPLANT_RUNTIME_IMAGES_READY) {
        SetLastError("Flutter app/runtime images are not ready for Dart method lookup");
        return DARTPLANT_RUNTIME_NOT_READY;
    }

    // The advanced bootstrap implementation remains available for diagnostics,
    // but the normal find-method path owns invoking it and exposing no
    // LiveVmContext/profile/register details to the consumer.
    status = dartplant_runtime_bootstrap_live_vm(state.runtime, nullptr, nullptr);
    if (status != DARTPLANT_OK) return status;

    artifact_status = BindRegisteredArtifactIndexIfReady(state.runtime);
    if (artifact_status != DARTPLANT_OK) return artifact_status;
    return DARTPLANT_OK;
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

DartPlantStatus BindCompilerEvidenceIfPresent(
    DartPlantRuntime* runtime, const std::vector<const DartPlantArtifactBundle*>& bundles,
    const DartPlantMethod* method) {
    if (bundles.empty() || method == nullptr) {
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
    for (const auto* bundle : bundles) {
        if (bundle == nullptr || bundle->compiler_abi_evidence == nullptr) continue;
        for (uint32_t index = 0; index < bundle->compiler_abi_evidence_count; ++index) {
            const auto& evidence = bundle->compiler_abi_evidence[index];
            if (!EvidenceMatchesMethod(evidence, method)) continue;
            matched = true;
            const DartPlantStatus status =
                dartplant_runtime_register_compiler_abi_evidence(runtime, method, &evidence);
            if (status == DARTPLANT_OK) continue;
            if (status == DARTPLANT_RUNTIME_NOT_READY) return status;
            // Compiler ABI evidence is an optional typed overlay. A stale,
            // unsupported or conflicting provider must never prevent the
            // already-resolved method from remaining usable through raw
            // invocation APIs.
            ClearLastError();
            return DARTPLANT_OK;
        }
    }
    if (matched) ClearLastError();
    return DARTPLANT_OK;
}

std::vector<const DartPlantArtifactBundle*> RegisteredArtifactBundles() {
    auto& registry = ArtifactRegistry();
    std::lock_guard lock(registry.mutex);
    return registry.bundles;
}

}  // namespace

bool DefaultRuntimeInitialized() {
    auto& state = DefaultRuntime();
    std::lock_guard lock(state.mutex);
    return state.runtime != nullptr;
}

DartPlantStatus BindRegisteredArtifactIndexIfReady(DartPlantRuntime* runtime) {
    return BindArtifactIndexIfReady(runtime, RegisteredArtifactBundles());
}

DartPlantStatus BindRegisteredCompilerEvidenceIfPresent(DartPlantRuntime* runtime,
                                                        const DartPlantMethod* method) {
    return BindCompilerEvidenceIfPresent(runtime, RegisteredArtifactBundles(), method);
}

DartPlantStatus FindDefaultRuntimeMethod(const DartPlantMethodQuery* query,
                                         DartPlantMethod** out_method) {
    auto& state = DefaultRuntime();
    std::lock_guard lock(state.mutex);
    DartPlantStatus status = EnsureDefaultRuntimeReady(state);
    if (status != DARTPLANT_OK) return status;
    status = dartplant_runtime_find_method(state.runtime, query, out_method);
    if (status != DARTPLANT_OK) return status;
    ClearLastError();
    return DARTPLANT_OK;
}

}  // namespace dartplant

extern "C" {

DartPlantStatus dartplant_init(const DartPlantInitInfo* info) {
    if (info == nullptr || info->struct_size < sizeof(DartPlantInitInfo) ||
        info->version != DARTPLANT_INIT_API_VERSION ||
        !dartplant::ValidArtifactBundle(info->artifact_bundle)) {
        dartplant::SetLastError("DartPlant init info is invalid or unsupported");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    auto& state = dartplant::DefaultRuntime();
    std::lock_guard lock(state.mutex);
    if (state.runtime != nullptr) return DARTPLANT_OK;

    if (info->host_api != nullptr) {
        const DartPlantStatus host_status = dartplant_install_host_api(info->host_api);
        if (host_status != DARTPLANT_OK) return host_status;
    }
    if (dartplant::State().host.binding.load(std::memory_order_acquire) == nullptr) {
        dartplant::SetLastError("DartPlant init requires a host hook backend");
        return DARTPLANT_HOST_API_UNAVAILABLE;
    }

    DartPlantRuntimeProfile profile{};
    dartplant_runtime_profile_init_arm64_aot(&profile);
    if (info->app_module_name != nullptr && info->app_module_name[0] != '\0') {
        profile.app_module_name = info->app_module_name;
    }
    if (info->runtime_module_name != nullptr && info->runtime_module_name[0] != '\0') {
        profile.runtime_module_name = info->runtime_module_name;
    }

    DartPlantRuntime* runtime = nullptr;
    const DartPlantStatus status = dartplant_runtime_create(&profile, &runtime);
    if (status != DARTPLANT_OK) return status;
    state.runtime = runtime;
    if (info->artifact_bundle != nullptr) {
        const DartPlantStatus artifact_status =
            dartplant_register_embedded_artifact_bundle(info->artifact_bundle);
        if (artifact_status != DARTPLANT_OK) {
            dartplant_runtime_destroy(runtime);
            state.runtime = nullptr;
            return artifact_status;
        }
    }

    dartplant::StartRuntimeModuleRefreshWorker(nullptr);
    dartplant::ScheduleRuntimeModuleRefresh();
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

void dartplant_shutdown(void) {
    auto& state = dartplant::DefaultRuntime();
    DartPlantRuntime* runtime = nullptr;
    {
        std::lock_guard lock(state.mutex);
        runtime = state.runtime;
        state.runtime = nullptr;
    }
    if (runtime != nullptr) dartplant_runtime_destroy(runtime);
}

uint8_t dartplant_is_initialized(void) { return dartplant::DefaultRuntimeInitialized() ? 1 : 0; }

DartPlantStatus dartplant_register_embedded_artifact_bundle(const DartPlantArtifactBundle* bundle) {
    if (!dartplant::ValidArtifactBundle(bundle) || bundle == nullptr) {
        dartplant::SetLastError("embedded DartPlant artifact bundle is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto& registry = dartplant::ArtifactRegistry();
    std::lock_guard lock(registry.mutex);
    if (std::find(registry.bundles.begin(), registry.bundles.end(), bundle) ==
        registry.bundles.end()) {
        registry.bundles.push_back(bundle);
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
