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
#include "runtime/runtime_image_set.h"
#include "runtime/snapshot_index.h"
#include "vm/abi/resolver.h"
#include "vm/object_bridge.h"
#include "vm/runtime_profiles.h"

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
    uint64_t image_id = 0;
    DartRuntimeOwnerIdentity owner{};
    uintptr_t code_target = 0;
    uint64_t generation = 0;
    uint32_t formal_parameter_count = 0;
    std::vector<abi::DartFunctionAbiEvidence> providers;
    abi::DartFunctionAbiResolution resolution;
    abi::DartCallLayoutStatus layout_status = abi::DartCallLayoutStatus::kIncompleteEvidence;
    std::shared_ptr<const abi::DartCallLayout> call_layout;
};

// Process -> Engine -> IsolateGroup is the ownership hierarchy exposed by the
// Dart VM itself. The process owns a collection of physical Flutter engines;
// each engine owns a collection of isolate-group semantic owners. Public APIs
// currently project one active engine/group at a time, but switching that
// projection must never overwrite the state retained by another owner.
struct RuntimeIsolateGroupState {
    // Generation is isolate-group scoped. A process may have multiple live
    // groups whose methods/hooks remain valid independently while another
    // group advances its own semantic generation.
    std::shared_ptr<std::atomic_uint64_t> generation = std::make_shared<std::atomic_uint64_t>(1);
    DartPlantRuntimeState runtime_state = DARTPLANT_RUNTIME_CREATED;
    bool profile_matched = false;
    DartPlantResolutionDiagnostics diagnostics{};
    bool retired = false;
    uint64_t incarnation_epoch = 0;
    uint64_t isolate_group_identity = 0;
    uint64_t isolate_generation = 0;
    std::optional<ModuleImage> app_module;
    std::optional<FlutterSnapshotSource> snapshot;
    RuntimeImageSet image_set;
    std::optional<SnapshotIndex> live_snapshot_index;
    std::optional<SnapshotIndex> artifact_snapshot_index;
    uint64_t bound_artifact_snapshot_generation = 0;
    DartPlantLiveVmFunctionIndexInfo live_function_index_info{};
    std::optional<DartPlantLiveVmContext> live_vm_context;
    vm_abi::CapabilityBindingSet capability_bindings;
    uint64_t live_vm_null_value = 0;
    uint64_t live_vm_bool_true_value = 0;
    uint64_t live_vm_bool_false_value = 0;
    DartEntryTargetRegistry entry_targets;
    std::vector<RuntimeAbiEvidenceEntry> abi_evidence;
};

struct RuntimeEngineState {
    RuntimeEngineState() { isolate_groups.push_back(std::make_unique<RuntimeIsolateGroupState>()); }

    uint64_t incarnation_epoch = 0;
    uintptr_t anchor = 0;
    bool retired = false;
    std::optional<ModuleImage> module;
    std::vector<std::unique_ptr<RuntimeIsolateGroupState>> isolate_groups;
    size_t active_isolate_group_index = 0;
};

struct RuntimeProcessState {
    RuntimeProcessState() { engines.push_back(std::make_unique<RuntimeEngineState>()); }

    std::vector<ModuleImage> modules;
    uint64_t next_engine_incarnation_epoch = 1;
    uint64_t next_isolate_group_incarnation_epoch = 1;
    std::vector<std::unique_ptr<RuntimeEngineState>> engines;
    size_t active_engine_index = 0;
};

void EraseRuntimeAbiEvidenceForImage(DartPlantRuntime* runtime, uint64_t image_id,
                                     uint64_t image_incarnation_epoch = 0);

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
bool SameRuntimeIsolateGroupSemanticContext(const DartPlantLiveVmContext& left,
                                            const DartPlantLiveVmContext& right);
bool IsCurrentRuntimeMethod(const DartPlantRuntime* runtime, const DartPlantMethod* method);
bool IsRuntimeMethodOwnerAlive(const DartPlantRuntime* runtime, const DartPlantMethod* method);
RuntimeOperationLease AcquireRuntimeOperation(const DartPlantRuntime* runtime);
// Internal host-test observation for deterministic close/drain regressions.
size_t RuntimeActiveOperationCountForTesting(const DartPlantRuntime* runtime);
DartPlantStatus RefreshRuntimeModules(DartPlantRuntime* runtime,
                                      const std::vector<ModuleImage>& modules);
DartPlantStatus RefreshRuntimeOwnerTree(DartPlantRuntime* runtime,
                                        const std::vector<ModuleImage>& modules);
void ActivateRuntimeEngineOwnerForAnchorLocked(DartPlantRuntime* runtime,
                                               const std::vector<ModuleImage>& modules,
                                               uintptr_t engine_anchor);
DartPlantStatus ActivateRuntimeIsolateGroupOwnerForContextLocked(
    DartPlantRuntime* runtime, const DartPlantLiveVmContext& context,
    const FlutterSnapshotSource& snapshot);
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
    dartplant::RuntimeProcessState process;
    // Compatibility projection of the active isolate-group generation token.
    // The token itself is owned by RuntimeIsolateGroupState and is swapped
    // when the active owner changes.
    std::shared_ptr<std::atomic_uint64_t> generation =
        process.engines[0]->isolate_groups[0]->generation;
    DartPlantResolutionDiagnostics diagnostics{};
    DartPlantRuntimeState state = DARTPLANT_RUNTIME_CREATED;
    bool profile_matched = false;
};

namespace dartplant {

inline RuntimeProcessState& RuntimeProcess(DartPlantRuntime* runtime) { return runtime->process; }
inline const RuntimeProcessState& RuntimeProcess(const DartPlantRuntime* runtime) {
    return runtime->process;
}
inline RuntimeEngineState& RuntimeEngine(DartPlantRuntime* runtime) {
    return *runtime->process.engines[runtime->process.active_engine_index];
}
inline const RuntimeEngineState& RuntimeEngine(const DartPlantRuntime* runtime) {
    return *runtime->process.engines[runtime->process.active_engine_index];
}
inline RuntimeIsolateGroupState& RuntimeIsolateGroup(DartPlantRuntime* runtime) {
    auto& engine = RuntimeEngine(runtime);
    return *engine.isolate_groups[engine.active_isolate_group_index];
}
inline const RuntimeIsolateGroupState& RuntimeIsolateGroup(const DartPlantRuntime* runtime) {
    const auto& engine = RuntimeEngine(runtime);
    return *engine.isolate_groups[engine.active_isolate_group_index];
}

inline void ProjectActiveGeneration(DartPlantRuntime* runtime) {
    runtime->generation = RuntimeIsolateGroup(runtime).generation;
}

inline void ProjectActiveRuntimeState(DartPlantRuntime* runtime) {
    const auto& group = RuntimeIsolateGroup(runtime);
    runtime->state = group.runtime_state;
    runtime->profile_matched = group.profile_matched;
    runtime->diagnostics = group.diagnostics;
}

inline void SetActiveRuntimeState(DartPlantRuntime* runtime, DartPlantRuntimeState state) {
    RuntimeIsolateGroup(runtime).runtime_state = state;
    runtime->state = state;
}

inline void SetActiveProfileMatched(DartPlantRuntime* runtime, bool matched) {
    RuntimeIsolateGroup(runtime).profile_matched = matched;
    runtime->profile_matched = matched;
}

inline DartPlantResolutionDiagnostics& RuntimeDiagnostics(DartPlantRuntime* runtime) {
    return RuntimeIsolateGroup(runtime).diagnostics;
}

inline const DartPlantResolutionDiagnostics& RuntimeDiagnostics(const DartPlantRuntime* runtime) {
    return RuntimeIsolateGroup(runtime).diagnostics;
}

inline void ProjectActiveDiagnostics(DartPlantRuntime* runtime) {
    runtime->diagnostics = RuntimeDiagnostics(runtime);
}

inline vm_abi::CapabilityOwnerStamp RuntimeCapabilityOwner(const DartPlantRuntime* runtime) {
    if (runtime == nullptr) return {};
    return {
        .runtime_generation = runtime->generation->load(std::memory_order_acquire),
        .engine_incarnation_epoch = RuntimeEngine(runtime).incarnation_epoch,
        .isolate_group_incarnation_epoch = RuntimeIsolateGroup(runtime).incarnation_epoch,
    };
}

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
    uint32_t closure_type_argument_root_base = 0;
    uint32_t closure_type_argument_root_count = 0;
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
