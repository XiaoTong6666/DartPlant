// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_CORE_INTERNAL_H_
#define DARTPLANT_CORE_INTERNAL_H_

#include <stdint.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "abi/call_layout.h"
#include "core/method_model.h"
#include "dartplant/advanced/runtime_profile.h"
#include "dartplant/dartplant.h"
#include "dartplant/host_api.h"
#include "dartplant/invocation.h"
#include "vm/object_bridge.h"

namespace dartplant {

struct ExecutableRange {
    uintptr_t start = 0;
    uintptr_t end = 0;
    uint64_t file_offset = 0;
    uint64_t virtual_address = 0;
    uint64_t file_size = 0;
};

struct ModuleImage {
    std::string name;
    std::string path;
    std::string build_id;
    uintptr_t load_bias = 0;
    std::vector<ExecutableRange> executable_ranges;

    bool ContainsExecutable(uintptr_t address, size_t size) const;
    std::optional<uintptr_t> Resolve(DartPlantAddressKind kind, uint64_t address,
                                     uint64_t section_va = 0) const;
};

struct ElfProgramHeaderView {
    uint32_t type = 0;
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint64_t virtual_address = 0;
    uint64_t file_size = 0;
    uint64_t memory_size = 0;
};

// Pure program-header parser shared by dl_iterate_phdr production discovery
// and the synthetic ELF regression corpus. It deliberately does not dereference
// mapped addresses or read build-id notes.
bool BuildModuleImageFromProgramHeaders(std::string_view path, uintptr_t load_bias,
                                        std::span<const ElfProgramHeaderView> headers,
                                        ModuleImage* out_image);

struct MethodRecord {
    std::string library_uri;
    std::string class_name;
    std::string function_name;
    std::string signature;
    DartPlantEntryKind entry_kind = DARTPLANT_ENTRY_DEFAULT;
    DartPlantAddressKind address_kind = DARTPLANT_ADDRESS_ELF_VA;
    uint64_t section_va = 0;
    uint64_t address = 0;
    uint32_t code_size = 0;
    std::string fingerprint;
};

struct MetadataIndex {
    uint32_t format = 0;
    std::string module_name;
    std::string build_id;
    std::string snapshot_hash;
    std::vector<MethodRecord> methods;
};

struct HostApiBinding {
    void* user_data = nullptr;
    DartPlantHostHookCallback hook = nullptr;
    DartPlantHostUnhookCallback unhook = nullptr;
    DartPlantHostHookWithPublicationCallback hook_with_publication = nullptr;
};

struct ManagedCodePatch {
    uintptr_t address = 0;
    std::vector<uint8_t> original_bytes;
    std::vector<uint8_t> patched_bytes;
};

struct HostApi {
    // Individual bindings are immutable/process-lifetime, while the current
    // default pointer may be replaced or cleared. Physical hooks retain the
    // immutable binding that created them, so shutdown/re-init cannot redirect
    // their eventual unhook callback.
    std::atomic<const HostApiBinding*> binding{nullptr};
};

enum class HookRecordState {
    kCreated,
    kInstalling,
    kInstalled,
    kUnhooking,
    kUnhooked,
    kFailed,
    // The backend restored its target after publishing our replacement. Keep
    // the callback veneer and HookRecord reachable for stale instruction fetch.
    kFailedAfterPublished,
    // The target mapping disappeared before the backend could safely restore
    // its bytes. Keep ownership and executable stubs for process lifetime.
    kRetired,
};

struct DartPlantListenerRecord {
    uint64_t id = 0;
    int32_t priority = 0;
    uint64_t registration_order = 0;
    DartPlantHookOptions options{};
    DartPlantVmAdapter* vm_adapter = nullptr;
    std::shared_ptr<DartPlantMethod> requested_method;
    std::atomic_bool active{true};
    std::atomic_uint64_t in_flight{0};
};

struct RuntimeState {
    std::mutex mutex;
    HostApi host;
    std::optional<MetadataIndex> metadata;
    std::vector<ModuleImage> modules;
    DartEntryTargetRegistry entry_targets;
};

RuntimeState& State();
void SetLastError(std::string message);
void ClearLastError();
const char* LastError();

std::optional<MetadataIndex> ParseMetadata(const char* json, std::string* error);
std::vector<ModuleImage> EnumerateModules();
std::optional<ModuleImage> FindModule(const std::vector<ModuleImage>& modules,
                                      const std::string& name);
std::string FingerprintCode(const void* address, size_t size);
std::string FingerprintCodeWithManagedPatches(const void* address, size_t size);

void InstallHostApi(const DartPlantHostApi* api);
void ClearHostApi(const HostApiBinding* expected_binding);
void RefreshModules();
void ReplaceModules(std::vector<ModuleImage> modules);
DartPlantStatus InvalidateRuntimeHooks(
    const std::shared_ptr<std::atomic_uint64_t>& runtime_generation);
void RetireRuntimeHooks(const std::shared_ptr<std::atomic_uint64_t>& runtime_generation);

DartPlantStatus InstallHook(const std::shared_ptr<DartEntryTarget>& code_target, void* replacement,
                            void** backup, DartPlantHook** out_hook);
DartPlantStatus InstallHook(uintptr_t target, void* replacement, void** backup,
                            DartPlantHook** out_hook);
DartPlantStatus InstallCallbackHook(const DartPlantMethod* method,
                                    const DartPlantRuntimeProfile& profile,
                                    const DartPlantHookOptions& options, int32_t priority,
                                    DartPlantHook** out_hook, DartPlantListener** out_listener,
                                    uint64_t validated_null_value = 0,
                                    std::shared_ptr<std::atomic_uint64_t> runtime_generation = {},
                                    uint64_t expected_runtime_generation = 0,
                                    uint64_t validated_bool_true_value = 0,
                                    uint64_t validated_bool_false_value = 0,
                                    std::shared_ptr<const abi::DartCallLayout> call_layout = {});
DartPlantStatus AddCallbackListener(
    DartPlantHook* hook, const DartPlantMethod* requested_method,
    const DartPlantHookOptions& options, int32_t priority, DartPlantListener** out_listener,
    const std::shared_ptr<std::atomic_uint64_t>& runtime_generation = {},
    uint64_t expected_runtime_generation = 0);
DartPlantStatus AddCallbackListenerForMethod(
    const DartPlantMethod* method, const DartPlantHookOptions& options, int32_t priority,
    DartPlantListener** out_listener,
    const std::shared_ptr<std::atomic_uint64_t>& runtime_generation = {},
    uint64_t expected_runtime_generation = 0);
bool BeginInvocation(DartPlantHook* hook,
                     std::vector<std::shared_ptr<DartPlantListenerRecord>>* listeners);
void* CreateArm64CallbackStub(DartPlantHook* hook, uintptr_t target, size_t* out_size);
void* CreateArm64PayloadReturnStub(DartCodePayload* payload, uintptr_t target, size_t* out_size);
void DestroyArm64CallbackStub(void* entry, size_t size);
bool CollectReachableArm64Returns(const uint8_t* code, size_t size, uintptr_t logical_start,
                                  std::vector<Arm64ReturnPatch>* out_returns);
DartPlantStatus InstallArm64ReturnInterception(DartPlantHook* hook);
bool RestoreArm64ReturnInterception(DartPlantHook* hook);
void RegisterArm64ExceptionBridgeConsumer(DartPlantHook* hook);
void ReleaseArm64ExceptionBridgeConsumer(DartPlantHook* hook);
bool EnsureArm64ExceptionBridge(DartPlantHook* hook, const DartPlantArm64Context& context);
DartPlantStatus RemoveHook(DartPlantHook* hook);
bool IsTargetHooked(uintptr_t target);
void ResetHooks();
void ReleaseHook(DartPlantHook* hook);
void InvocationExited(DartPlantHook* hook);

}  // namespace dartplant

struct DartPlantMethod {
    dartplant::MethodRecord record;
    dartplant::ModuleImage module;
    std::shared_ptr<dartplant::DartFunctionHandle> function;
    std::shared_ptr<std::atomic_uint64_t> runtime_generation;
    uint64_t expected_runtime_generation = 0;
};

namespace dartplant {

inline DartMethodIdentity MethodIdentityFromRecord(const MethodRecord& record) {
    return {
        .library_uri = record.library_uri,
        .class_name = record.class_name,
        .function_name = record.function_name,
        .signature = record.signature,
        .entry_kind = record.entry_kind,
    };
}

inline uintptr_t MethodTarget(const DartPlantMethod* method) {
    return method == nullptr || method->function == nullptr ||
                   method->function->code_target == nullptr
               ? 0
               : method->function->code_target->entry;
}

inline uint32_t MethodCodeSize(const DartPlantMethod* method) {
    return method == nullptr || method->function == nullptr ||
                   method->function->code_target == nullptr
               ? 0
               : method->function->code_target->code_size;
}

}  // namespace dartplant

struct DartPlantHook {
    mutable std::recursive_mutex mutex;
    std::shared_ptr<dartplant::DartEntryTarget> code_target;
    std::atomic<void*> backup{nullptr};
    std::atomic_bool active{false};
    bool has_method = false;
    bool shared_code_opt_in = false;
    std::unique_ptr<DartPlantMethod> method_storage;
    DartPlantRuntimeProfile profile{};
    dartplant::HookRecordState state = dartplant::HookRecordState::kCreated;
    uint64_t next_listener_id = 1;
    uint64_t next_registration_order = 1;
    uint64_t in_flight = 0;
    uint64_t listener_handles = 0;
    bool release_requested = false;
    std::vector<std::shared_ptr<dartplant::DartPlantListenerRecord>> listeners;
    DartPlantHookOptions options{};
    DartPlantVmAdapter* vm_adapter = nullptr;
    uint64_t validated_null_value = 0;
    uint64_t validated_bool_true_value = 0;
    uint64_t validated_bool_false_value = 0;
    std::shared_ptr<const dartplant::abi::DartCallLayout> call_layout;
    const dartplant::HostApiBinding* host_binding = nullptr;
    // Backend entry-patch ownership and payload-return ownership are
    // independent transactions. A failed callback install/unhook can restore
    // one side before the other, so retries must never infer backend ownership
    // from HookRecordState alone.
    std::atomic_bool backend_installed{false};
    // Published by the core only after the host has returned a visible backup
    // trampoline. Callback veneers treat installing/not-ready as passthrough.
    std::atomic_bool entry_published{false};
    std::atomic_bool entry_ready{false};
    std::shared_ptr<std::atomic_uint64_t> runtime_generation;
    uint64_t expected_runtime_generation = 0;
    void* replacement_entry = nullptr;
    size_t replacement_entry_size = 0;
    bool payload_return_consumer = false;
    std::vector<uintptr_t> payload_return_sites;
    std::vector<dartplant::ManagedCodePatch> managed_backend_patches;
    bool exception_bridge_consumer = false;
    bool vm_adapter_retained = false;
};

struct DartPlantListener {
    DartPlantHook* hook = nullptr;
    std::shared_ptr<dartplant::DartPlantListenerRecord> record;
};

struct DartPlantHookHandle {
    DartPlantListener* listener = nullptr;
    bool removed = false;
};

#endif  // DARTPLANT_CORE_INTERNAL_H_
