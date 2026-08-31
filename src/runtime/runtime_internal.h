// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_RUNTIME_RUNTIME_INTERNAL_H_
#define DARTPLANT_RUNTIME_RUNTIME_INTERNAL_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "abi/call_layout.h"
#include "abi/evidence.h"
#include "core/internal.h"
#include "dartplant/advanced/live_vm.h"
#include "dartplant/invocation.h"
#include "dartplant/runtime.h"
#include "runtime/flutter_snapshot_internal.h"
#include "runtime/snapshot_index.h"
#include "vm/object_bridge.h"

namespace dartplant {

struct RuntimeProfileStorage {
    DartPlantRuntimeProfile profile{};
    std::string profile_name;
    std::string dart_version;
    std::string flutter_version;
    std::string app_module_name;
    std::string app_build_id;
    std::string runtime_module_name;
    std::string runtime_build_id;

    void Assign(const DartPlantRuntimeProfile& source);
};

struct RuntimeRegistration;

struct RuntimeAbiEvidenceEntry {
    DartMethodIdentity identity;
    uintptr_t code_target = 0;
    uint64_t generation = 0;
    uint32_t formal_parameter_count = 0;
    std::vector<abi::DartFunctionAbiEvidence> providers;
    abi::DartFunctionAbiResolution resolution;
    abi::DartCallLayoutStatus layout_status = abi::DartCallLayoutStatus::kIncompleteEvidence;
    std::shared_ptr<const abi::DartCallLayout> call_layout;
};

using RuntimeModuleRefreshReporter = void (*)(DartPlantStatus status, const char* error);

struct RuntimeOperationLease {
    std::shared_ptr<RuntimeRegistration> registration;

    RuntimeOperationLease() = default;
    RuntimeOperationLease(const RuntimeOperationLease&) = delete;
    RuntimeOperationLease& operator=(const RuntimeOperationLease&) = delete;
    RuntimeOperationLease(RuntimeOperationLease&& other) noexcept;
    RuntimeOperationLease& operator=(RuntimeOperationLease&& other) noexcept;
    ~RuntimeOperationLease();

    explicit operator bool() const { return registration != nullptr; }
};

bool EqualsIgnoreCaseAscii(const std::string& left, const std::string& right);
bool IsCurrentRuntimeMethod(const DartPlantRuntime* runtime, const DartPlantMethod* method);
RuntimeOperationLease AcquireRuntimeOperation(const DartPlantRuntime* runtime);
// Internal host-test observation for deterministic close/drain regressions.
size_t RuntimeActiveOperationCountForTesting(const DartPlantRuntime* runtime);
DartPlantStatus RefreshRuntimeModules(DartPlantRuntime* runtime,
                                      const std::vector<ModuleImage>& modules);
void StartRuntimeModuleRefreshWorker(RuntimeModuleRefreshReporter reporter);
uint64_t ScheduleRuntimeModuleRefresh();
DartPlantStatus WaitForRuntimeModuleRefresh(uint64_t epoch);
DartPlantStatus ResolveLiveVmCanonicalBoolRoots(const DartPlantLiveVmContext& context,
                                                const DartPlantLiveVmProfile& profile,
                                                uint64_t* out_true, uint64_t* out_false);
std::shared_ptr<const abi::DartCallLayout> FindRuntimeCallLayoutLocked(
    const DartPlantRuntime* runtime, const DartPlantMethod* method);
void SetRuntimeDiagnostics(DartPlantRuntime* runtime, DartPlantResolveStage stage,
                           DartPlantResolveOutcome outcome, DartPlantStatus status,
                           DartPlantResolveRejectReason reject_reason = DARTPLANT_REJECT_NONE);

}  // namespace dartplant

typedef struct DartPlantArm64DispatchResult {
    DartPlantArm64Context* context;
    void* original;
} DartPlantArm64DispatchResult;

typedef struct DartPlantArm64LeaveResult {
    DartPlantArm64Context* context;
    uint64_t result;
} DartPlantArm64LeaveResult;

typedef struct DartPlantArm64ReturnDispatchResult {
    DartPlantArm64Context* context;
    uintptr_t resume_native_sp;
} DartPlantArm64ReturnDispatchResult;

struct DartPlantRuntime {
    mutable std::recursive_mutex mutex;
    dartplant::RuntimeProfileStorage profile;
    std::vector<dartplant::ModuleImage> modules;
    std::optional<dartplant::ModuleImage> selected_app_module;
    std::optional<dartplant::ModuleImage> selected_runtime_module;
    std::optional<dartplant::FlutterSnapshotSource> snapshot;
    // Built automatically from live Class.functions/Library.toplevel_class.
    std::optional<dartplant::SnapshotIndex> live_snapshot_index;
    // Optional exact compiler/artifact sidecar for Functions deliberately
    // dropped from the PRODUCT object graph. Bound to one app/snapshot
    // incarnation and cleared when that artifact identity changes.
    std::optional<dartplant::SnapshotIndex> artifact_snapshot_index;
    uint64_t bound_artifact_snapshot_generation = 0;
    DartPlantLiveVmFunctionIndexInfo live_function_index_info{};
    std::optional<DartPlantLiveVmContext> live_vm_context;
    // Canonical semantic roots are captured only from an exact, validated live
    // VM profile and are scoped to this runtime generation.
    uint64_t live_vm_null_value = 0;
    uint64_t live_vm_bool_true_value = 0;
    uint64_t live_vm_bool_false_value = 0;
    std::shared_ptr<std::atomic_uint64_t> generation = std::make_shared<std::atomic_uint64_t>(1);
    dartplant::DartEntryTargetRegistry entry_targets;
    std::vector<dartplant::RuntimeAbiEvidenceEntry> abi_evidence;
    DartPlantResolutionDiagnostics diagnostics{};
    DartPlantRuntimeState state = DARTPLANT_RUNTIME_CREATED;
    bool profile_matched = false;
};

namespace dartplant {

inline bool IsArtifactRuntimeMethod(const DartPlantMethod* method) {
    return method != nullptr && method->function != nullptr &&
           method->function->source == DartFunctionSource::kOfflineSnapshotIndex;
}

inline bool RuntimeReadyForMethodOperation(const DartPlantRuntime* runtime,
                                           const DartPlantMethod* method) {
    if (runtime == nullptr) return false;
    if (runtime->state == DARTPLANT_RUNTIME_READY) return true;
    return runtime->state == DARTPLANT_RUNTIME_IMAGES_READY && IsArtifactRuntimeMethod(method);
}

DartPlantStatus ReplaceRuntimeArtifactSnapshotIndex(DartPlantRuntime* runtime,
                                                    const DartPlantSnapshotIndexInfo* source,
                                                    uint64_t registry_generation);

}  // namespace dartplant

struct DartPlantInvocation {
    DartPlantHook* hook = nullptr;
    const DartPlantMethod* requested_method = nullptr;
    std::shared_ptr<dartplant::DartEntryTarget> code_target;
    std::vector<dartplant::DartMethodIdentity> code_alias_snapshot;
    const DartPlantRuntimeProfile* profile = nullptr;
    const dartplant::abi::DartCallLayout* call_layout = nullptr;
    DartPlantArm64Context* context = nullptr;
    DartPlantInvocationPhase phase = DARTPLANT_INVOCATION_ENTER;
    uint32_t depth = 0;
    DartPlantVmAdapter* vm_adapter = nullptr;
    uint64_t validated_null_value = 0;
    uint64_t validated_bool_true_value = 0;
    uint64_t validated_bool_false_value = 0;
    uint64_t live_vm_heap_base = 0;
    struct GeneratedRootAccess {
        dartplant::abi::DartAbiLocation location{};
        uint32_t root_index = 0;
        bool is_result = false;
    };
    void* generated_root_lease = nullptr;
    std::vector<GeneratedRootAccess> generated_root_accesses;
    bool identity_ambiguous = false;
    bool closure_receiver_in_x0 = false;
    bool vm_scope_entered = false;
    bool generated_vm_bridge_active = false;
    mutable std::vector<dartplant::abi::DartParameterLayout> mapped_parameters;
    mutable dartplant::abi::DartAbiLocation closure_type_arguments_location{};
    mutable bool closure_argument_mapping_attempted = false;
    mutable bool closure_argument_mapping_valid = false;
    bool skip_original = false;
    bool call_original = false;
    bool original_called = false;
    std::vector<std::shared_ptr<dartplant::DartPlantListenerRecord>> entered_listeners;
};

const std::vector<dartplant::abi::DartParameterLayout>* InvocationParameters(
    const DartPlantInvocation* invocation);

extern "C" DartPlantArm64DispatchResult dartplant_arm64_dispatch_enter(
    DartPlantArm64Context* context, DartPlantHook* hook);

extern "C" uint8_t dartplant_arm64_invoke_original(DartPlantArm64Context* context, void* original);

extern "C" uint8_t dartplant_arm64_prepare_invoke_original_frame(uintptr_t native_frame_sp);

extern "C" DartPlantArm64ReturnDispatchResult dartplant_arm64_dispatch_return_from_hook(
    DartPlantHook* hook, uint64_t result0, uint64_t result1, uint64_t fp_result_bits,
    uintptr_t return_lr, uintptr_t return_spreg, uintptr_t return_fp);

extern "C" DartPlantArm64ReturnDispatchResult dartplant_arm64_dispatch_return_from_payload(
    dartplant::DartCodePayload* payload, uint64_t result0, uint64_t result1,
    uint64_t fp_result_bits, uintptr_t return_lr, uintptr_t return_spreg, uintptr_t return_fp);

extern "C" void dartplant_arm64_dispatch_exception_unwind(uintptr_t target_spreg,
                                                          uintptr_t target_fp);

extern "C" DartPlantArm64LeaveResult dartplant_arm64_dispatch_leave_from_tls(
    uint64_t result0, uint64_t result1, uint64_t fp_result_bits);

#endif  // DARTPLANT_RUNTIME_RUNTIME_INTERNAL_H_
