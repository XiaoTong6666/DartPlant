// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cctype>
#include <cstring>
#include <new>

#if defined(DARTPLANT_USE_PTHREAD_TLS)
#include <pthread.h>
#endif

#include "core/internal.h"
#include "runtime/default_runtime.h"

namespace dartplant {
namespace {

#if defined(DARTPLANT_USE_PTHREAD_TLS)
pthread_key_t g_last_error_key;
pthread_once_t g_last_error_key_once = PTHREAD_ONCE_INIT;
bool g_last_error_key_ready = false;

void DestroyLastError(void* value) { delete static_cast<std::string*>(value); }

void CreateLastErrorKey() {
    if (pthread_key_create(&g_last_error_key, DestroyLastError) == 0) {
        g_last_error_key_ready = true;
    }
}

std::string* LastErrorStorage() {
    pthread_once(&g_last_error_key_once, CreateLastErrorKey);
    if (!g_last_error_key_ready) return nullptr;
    auto* value = static_cast<std::string*>(pthread_getspecific(g_last_error_key));
    if (value != nullptr) return value;
    value = new (std::nothrow) std::string();
    if (value == nullptr || pthread_setspecific(g_last_error_key, value) != 0) {
        delete value;
        return nullptr;
    }
    return value;
}
#else
thread_local std::string g_last_error;

std::string* LastErrorStorage() { return &g_last_error; }
#endif

bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
                                                     [](unsigned char a, unsigned char b) {
                                                         return std::tolower(a) == std::tolower(b);
                                                     });
}

bool Matches(const std::string& actual, const char* expected) {
    return actual.empty() || expected == nullptr || expected[0] == '\0' || actual == expected;
}

DartPlantStatus ValidateTarget(const ModuleImage& module, uintptr_t target, uint32_t code_size,
                               const char* expected_build_id, const char* expected_fingerprint) {
    if (code_size == 0 || !module.ContainsExecutable(target, code_size)) {
        SetLastError("target is outside the module executable ranges");
        return DARTPLANT_ADDRESS_OUTSIDE_EXECUTABLE;
    }
    if (expected_build_id != nullptr && expected_build_id[0] != '\0' &&
        !EqualsIgnoreCase(module.build_id, expected_build_id)) {
        SetLastError("module build-id does not match metadata");
        return DARTPLANT_BUILD_ID_MISMATCH;
    }
    if (expected_fingerprint != nullptr && expected_fingerprint[0] != '\0' &&
        FingerprintCode(reinterpret_cast<const void*>(target), code_size) != expected_fingerprint) {
        SetLastError("code fingerprint does not match metadata");
        return DARTPLANT_FINGERPRINT_MISMATCH;
    }
    return DARTPLANT_OK;
}

}  // namespace

void SetLastError(std::string message) {
    if (std::string* storage = LastErrorStorage(); storage != nullptr) {
        *storage = std::move(message);
    }
}

void ClearLastError() {
    if (std::string* storage = LastErrorStorage(); storage != nullptr) storage->clear();
}

const char* LastError() {
    if (std::string* storage = LastErrorStorage(); storage != nullptr) return storage->c_str();
    return "DartPlant per-thread error storage is unavailable";
}

}  // namespace dartplant

extern "C" {

DartPlantStatus dartplant_install_host_api(const DartPlantHostApi* api) {
    if (api == nullptr || api->struct_size < sizeof(DartPlantHostApi) ||
        api->version < DARTPLANT_HOST_API_VERSION || api->hook == nullptr ||
        api->unhook == nullptr) {
        dartplant::SetLastError("host API version or function pointers are invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    dartplant::InstallHostApi(api);
    return DARTPLANT_OK;
}

DartPlantStatus dartplant_initialize_from_json(const char* metadata_json) {
    std::string error;
    auto metadata = dartplant::ParseMetadata(metadata_json, &error);
    if (!metadata.has_value()) {
        dartplant::SetLastError(error);
        return DARTPLANT_METADATA_INVALID;
    }
    auto modules = dartplant::EnumerateModules();
    auto& state = dartplant::State();
    std::lock_guard lock(state.mutex);
    state.metadata = std::move(metadata);
    state.modules = std::move(modules);
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

void dartplant_reset(void) {
    dartplant_shutdown();
    dartplant::ResetHooks();
    auto& state = dartplant::State();
    std::lock_guard lock(state.mutex);
    state.metadata.reset();
    state.modules.clear();
    state.code_targets.Clear();
    dartplant::ClearLastError();
}

const char* dartplant_last_error(void) { return dartplant::LastError(); }

DartPlantStatus dartplant_find_method(const DartPlantMethodQuery* query,
                                      DartPlantMethod** out_method) {
    if (query == nullptr || out_method == nullptr ||
        query->struct_size < sizeof(DartPlantMethodQuery) || query->library_uri == nullptr ||
        query->function_name == nullptr) {
        dartplant::SetLastError("method query is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }

    // The normal LSPlant-style API resolves through the internally managed
    // runtime. Legacy JSON metadata remains available only when dartplant_init()
    // has not created that default runtime.
    if (dartplant::DefaultRuntimeInitialized()) {
        return dartplant::FindDefaultRuntimeMethod(query, out_method);
    }

    auto& state = dartplant::State();
    std::lock_guard lock(state.mutex);
    if (!state.metadata.has_value()) {
        dartplant::SetLastError("metadata is not initialized");
        return DARTPLANT_NOT_INITIALIZED;
    }
    const auto module = dartplant::FindModule(state.modules, state.metadata->module_name);
    if (!module.has_value()) {
        dartplant::SetLastError("metadata module is not loaded");
        return DARTPLANT_MODULE_NOT_FOUND;
    }
    if (!state.metadata->build_id.empty() &&
        !dartplant::EqualsIgnoreCase(module->build_id, state.metadata->build_id)) {
        dartplant::SetLastError("loaded module build-id does not match metadata");
        return DARTPLANT_BUILD_ID_MISMATCH;
    }

    std::vector<const dartplant::MethodRecord*> matches;
    for (const auto& method : state.metadata->methods) {
        if (method.library_uri == query->library_uri &&
            method.function_name == query->function_name &&
            dartplant::Matches(method.class_name, query->class_name) &&
            dartplant::Matches(method.signature, query->signature) &&
            method.entry_kind == query->entry_kind) {
            matches.push_back(&method);
        }
    }
    if (matches.empty()) {
        dartplant::SetLastError("Dart method was not found in metadata");
        return DARTPLANT_METHOD_NOT_FOUND;
    }
    if (matches.size() != 1) {
        dartplant::SetLastError("Dart method query is ambiguous");
        return DARTPLANT_AMBIGUOUS_METHOD;
    }
    const auto target = module->Resolve(matches[0]->address_kind, matches[0]->address);
    if (!target.has_value()) {
        dartplant::SetLastError("method address kind cannot be resolved");
        return DARTPLANT_UNSUPPORTED_ADDRESS_KIND;
    }
    const DartPlantStatus validation = dartplant::ValidateTarget(
        *module, *target, matches[0]->code_size, state.metadata->build_id.c_str(),
        matches[0]->fingerprint.c_str());
    if (validation != DARTPLANT_OK) return validation;

    auto code_target = state.code_targets.GetOrCreate(*target, matches[0]->code_size);
    if (code_target == nullptr) {
        dartplant::SetLastError("method resolver produced an invalid code target");
        return DARTPLANT_METHOD_NOT_FOUND;
    }
    auto function = std::make_shared<dartplant::DartFunctionHandle>();
    function->identity = dartplant::MethodIdentityFromRecord(*matches[0]);
    function->source = dartplant::DartFunctionSource::kLegacyMetadata;
    function->code_target = code_target;

    auto* method = new DartPlantMethod;
    method->record = *matches[0];
    method->module = *module;
    method->function = std::move(function);
    *out_method = method;
    dartplant::ClearLastError();
    return DARTPLANT_OK;
}

void dartplant_release_method(DartPlantMethod* method) { delete method; }

uintptr_t dartplant_method_runtime_address(const DartPlantMethod* method) {
    return dartplant::MethodTarget(method);
}

DartPlantStatus dartplant_hook_method_raw(const DartPlantMethod* method, void* replacement,
                                          void** backup, DartPlantHook** out_hook) {
    if (method == nullptr || method->function == nullptr ||
        method->function->code_target == nullptr) {
        dartplant::SetLastError("method has no code target");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    return dartplant::InstallHook(method->function->code_target, replacement, backup, out_hook);
}

DartPlantStatus dartplant_hook_address(const DartPlantAddressQuery* query, void* replacement,
                                       void** backup, DartPlantHook** out_hook) {
    if (query == nullptr || query->struct_size < sizeof(DartPlantAddressQuery) ||
        query->module_name == nullptr) {
        dartplant::SetLastError("address query is invalid");
        return DARTPLANT_INVALID_ARGUMENT;
    }
    auto& state = dartplant::State();
    std::unique_lock lock(state.mutex);
    const auto module = dartplant::FindModule(state.modules, query->module_name);
    if (!module.has_value()) {
        lock.unlock();
        dartplant::RefreshModules();
        lock.lock();
    }
    const auto refreshed = dartplant::FindModule(state.modules, query->module_name);
    if (!refreshed.has_value()) {
        dartplant::SetLastError("address query module is not loaded");
        return DARTPLANT_MODULE_NOT_FOUND;
    }
    const auto target = refreshed->Resolve(query->address_kind, query->address);
    if (!target.has_value()) {
        dartplant::SetLastError("address kind cannot be resolved");
        return DARTPLANT_UNSUPPORTED_ADDRESS_KIND;
    }
    const DartPlantStatus validation =
        dartplant::ValidateTarget(*refreshed, *target, query->code_size, query->expected_build_id,
                                  query->expected_fingerprint);
    lock.unlock();
    if (validation != DARTPLANT_OK) return validation;
    return dartplant::InstallHook(*target, replacement, backup, out_hook);
}

DartPlantStatus dartplant_unhook(DartPlantHook* hook) { return dartplant::RemoveHook(hook); }

uint8_t dartplant_is_hooked(const DartPlantMethod* method) {
    if (method == nullptr) return 0;
    auto& state = dartplant::State();
    std::lock_guard lock(state.mutex);
    return dartplant::IsTargetHooked(dartplant::MethodTarget(method)) ? 1 : 0;
}

void dartplant_release_hook(DartPlantHook* hook) { dartplant::ReleaseHook(hook); }

}  // extern "C"
