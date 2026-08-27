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

#include "core/internal.h"
#include "dartplant/invocation.h"
#include "dartplant/live_vm.h"
#include "dartplant/runtime.h"
#include "runtime/flutter_snapshot_internal.h"
#include "runtime/snapshot_index.h"
#include "runtime/vm_adapter_internal.h"

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
DartPlantStatus RefreshRuntimeModules(DartPlantRuntime* runtime,
                                      const std::vector<ModuleImage>& modules);
DartPlantStatus NotifyRuntimeModuleLoaded(const char* module_name, void* module_handle);

}  // namespace dartplant

typedef struct DartPlantArm64DispatchResult {
    DartPlantArm64Context* context;
    void* original;
} DartPlantArm64DispatchResult;

typedef struct DartPlantArm64LeaveResult {
    DartPlantArm64Context* context;
    uint64_t result;
} DartPlantArm64LeaveResult;

struct DartPlantRuntime {
    mutable std::recursive_mutex mutex;
    dartplant::RuntimeProfileStorage profile;
    std::vector<dartplant::ModuleImage> modules;
    std::optional<dartplant::ModuleImage> selected_app_module;
    std::optional<dartplant::ModuleImage> selected_runtime_module;
    std::optional<dartplant::FlutterSnapshotSource> snapshot;
    // Built automatically from live Class.functions/Library.toplevel_class.
    // Runtime method resolution never consumes a precomputed metadata/index file.
    std::optional<dartplant::SnapshotIndex> live_snapshot_index;
    DartPlantLiveVmFunctionIndexInfo live_function_index_info{};
    std::optional<DartPlantLiveVmContext> live_vm_context;
    // The canonical null heap object is captured only from a semantically
    // validated Dart NULL_REG sample. It never crosses the public C ABI.
    uint64_t live_vm_null_value = 0;
    std::shared_ptr<std::atomic_uint64_t> generation = std::make_shared<std::atomic_uint64_t>(1);
    dartplant::DartCodeTargetRegistry code_targets;
    DartPlantRuntimeState state = DARTPLANT_RUNTIME_CREATED;
    bool profile_matched = false;
};

struct DartPlantInvocation {
    DartPlantHook* hook = nullptr;
    const DartPlantMethod* requested_method = nullptr;
    std::shared_ptr<dartplant::DartCodeTarget> code_target;
    std::vector<dartplant::DartMethodIdentity> code_alias_snapshot;
    const DartPlantRuntimeProfile* profile = nullptr;
    DartPlantArm64Context* context = nullptr;
    DartPlantInvocationPhase phase = DARTPLANT_INVOCATION_ENTER;
    uint32_t depth = 0;
    DartPlantVmAdapter* vm_adapter = nullptr;
    uint64_t validated_null_value = 0;
    bool identity_ambiguous = false;
    bool vm_scope_entered = false;
    bool skip_original = false;
    bool call_original = false;
    bool original_called = false;
    std::vector<std::shared_ptr<dartplant::DartPlantListenerRecord>> entered_listeners;
};

extern "C" DartPlantArm64DispatchResult dartplant_arm64_dispatch_enter(
    DartPlantArm64Context* context, DartPlantHook* hook);

extern "C" uint64_t dartplant_arm64_invoke_original(DartPlantArm64Context* context, void* original);

extern "C" DartPlantArm64LeaveResult dartplant_arm64_dispatch_leave_from_tls(uint64_t result);

#endif  // DARTPLANT_RUNTIME_RUNTIME_INTERNAL_H_
