// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ADAPTERS_FLUTTER_VM_H_
#define DARTPLANT_ADAPTERS_FLUTTER_VM_H_

#include <stdint.h>

#include "dartplant/dartplant.h"
#include "dartplant/vm_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DARTPLANT_FLUTTER_VM_ADAPTER_API_VERSION 1u

typedef struct DartPlantFlutterVmAdapter DartPlantFlutterVmAdapter;

typedef enum DartPlantFlutterVmCapability {
    DARTPLANT_FLUTTER_VM_CAP_RUNTIME_ROOTS_PROVEN = DARTPLANT_VM_CAP_RUNTIME_ROOTS,
    DARTPLANT_FLUTTER_VM_CAP_OWNER_IDENTITY_PROVEN = DARTPLANT_VM_CAP_OWNER_IDENTITY,
    DARTPLANT_FLUTTER_VM_CAP_CANONICAL_NULL_PROVEN = DARTPLANT_VM_CAP_CANONICAL_NULL,
    DARTPLANT_FLUTTER_VM_CAP_REGISTER_SEMANTICS_PROVEN = DARTPLANT_VM_CAP_REGISTER_SEMANTICS,
    DARTPLANT_FLUTTER_VM_CAP_DART_CORE_PROVEN = DARTPLANT_VM_CAP_DART_CORE,
    DARTPLANT_FLUTTER_VM_CAP_SAFEPOINT_STUBS_PROVEN = DARTPLANT_VM_CAP_SAFEPOINT_STUBS,
    DARTPLANT_FLUTTER_VM_CAP_GENERATED_TRANSITION_SOURCE_VERIFIED =
        DARTPLANT_VM_CAP_GENERATED_TRANSITION_LAYOUT,
    DARTPLANT_FLUTTER_VM_CAP_EXCEPTION_SOURCE_VERIFIED = DARTPLANT_VM_CAP_EXCEPTION_LAYOUT,
    DARTPLANT_FLUTTER_VM_CAP_TYPE_ARGUMENTS_SOURCE_VERIFIED =
        DARTPLANT_VM_CAP_TYPE_ARGUMENTS_LAYOUT,
    DARTPLANT_FLUTTER_VM_CAP_ARTIFACT_LIFECYCLE_BOUND = DARTPLANT_VM_CAP_ARTIFACT_LIFECYCLE,
    DARTPLANT_FLUTTER_VM_CAP_ARGUMENTS_DESCRIPTOR_LAYOUT_VERIFIED =
        DARTPLANT_VM_CAP_ARGUMENTS_DESCRIPTOR_LAYOUT,
    DARTPLANT_FLUTTER_VM_CAP_INVOCATION_CALL_ABI_VERIFIED = DARTPLANT_VM_CAP_INVOCATION_CALL_ABI,
    DARTPLANT_FLUTTER_VM_CAP_FUNCTION_CODE_LAYOUT_VERIFIED = DARTPLANT_VM_CAP_FUNCTION_CODE_LAYOUT,
    DARTPLANT_FLUTTER_VM_CAP_AOT_ENTRY_LAYOUT_VERIFIED = DARTPLANT_VM_CAP_AOT_ENTRY_LAYOUT,
    DARTPLANT_FLUTTER_VM_CAP_FUNCTION_TYPE_LAYOUT_VERIFIED = DARTPLANT_VM_CAP_FUNCTION_TYPE_LAYOUT,
    DARTPLANT_FLUTTER_VM_CAP_CLOSURE_CALL_LAYOUT_VERIFIED = DARTPLANT_VM_CAP_CLOSURE_CALL_LAYOUT,
    DARTPLANT_FLUTTER_VM_CAP_EXCEPTION_BRIDGE_LAYOUT_VERIFIED =
        DARTPLANT_VM_CAP_EXCEPTION_BRIDGE_LAYOUT,
} DartPlantFlutterVmCapability;

typedef enum DartPlantFlutterVmProofState {
    DARTPLANT_FLUTTER_VM_PROOF_UNSUPPORTED = 0,
    DARTPLANT_FLUTTER_VM_PROOF_UNVERIFIED = 1,
    DARTPLANT_FLUTTER_VM_PROOF_VERIFIED = 2,
    DARTPLANT_FLUTTER_VM_PROOF_FAILED_FOR_INCARNATION = 3,
    DARTPLANT_FLUTTER_VM_PROOF_AMBIGUOUS = 4,
} DartPlantFlutterVmProofState;

typedef struct DartPlantFlutterVmAdapterOptions {
    uint32_t struct_size;
    uint32_t api_version;
    void* api_dl_data;
    uint64_t thread;
    uint64_t isolate_generation;
    const char* snapshot_hash;
    const char* snapshot_features;
} DartPlantFlutterVmAdapterOptions;

typedef struct DartPlantFlutterVmDescriptor {
    uint32_t struct_size;
    uint32_t descriptor_version;
    uint32_t vm_adapter_version;
    const char* descriptor_id;
    const char* dart_version;
    const char* flutter_version;
    const char* snapshot_hash;
    const char* flutter_module_name;
    // Deprecated compatibility field. Build IDs are discovered dynamically
    // from mapped ELF artifacts and bind only the artifact incarnation.
    const char* flutter_build_id;
    uint32_t pointer_size;
    uint8_t compressed_pointers;
    uint8_t product_mode;
    uint8_t reserved[2];
} DartPlantFlutterVmDescriptor;

DARTPLANT_EXPORT DartPlantStatus dartplant_flutter_vm_adapter_create(
    const DartPlantFlutterVmAdapterOptions* options, DartPlantFlutterVmAdapter** out_instance);
DARTPLANT_EXPORT DartPlantVmAdapter* dartplant_flutter_vm_adapter_get(
    DartPlantFlutterVmAdapter* instance);
DARTPLANT_EXPORT const DartPlantFlutterVmDescriptor* dartplant_flutter_vm_adapter_descriptor(
    const DartPlantFlutterVmAdapter* instance);
DARTPLANT_EXPORT uint64_t
dartplant_flutter_vm_adapter_capabilities(const DartPlantFlutterVmAdapter* instance);
DARTPLANT_EXPORT uint64_t
dartplant_flutter_vm_adapter_verified_capabilities(const DartPlantFlutterVmAdapter* instance);
DARTPLANT_EXPORT uint64_t
dartplant_flutter_vm_adapter_failed_capabilities(const DartPlantFlutterVmAdapter* instance);
DARTPLANT_EXPORT DartPlantFlutterVmProofState dartplant_flutter_vm_adapter_capability_state(
    const DartPlantFlutterVmAdapter* instance, DartPlantFlutterVmCapability capability);
DARTPLANT_EXPORT uint64_t
dartplant_flutter_vm_adapter_artifact_generation(const DartPlantFlutterVmAdapter* instance);
// Marks the current binding unusable before a loader/unload/restart event.
// This is an immediate poison operation, not a callback/transition quiescence
// barrier. It closes new VM-adapter admission and serializes with any safepoint
// stub currently executing, but callers must not treat its return as permission
// to unmap old AOT code while hooks/callbacks/root leases may still be alive.
// For loader teardown, prefer quiesce_artifacts() followed by
// retire_artifacts(), or call retire_artifacts() repeatedly until it returns OK.
// This never changes the selected ABI or captured artifact identity.
DARTPLANT_EXPORT void dartplant_flutter_vm_adapter_invalidate_artifacts(
    DartPlantFlutterVmAdapter* instance);
// Closes new callback/hook/VM-scope admission and starts logical unhook of every
// hook retaining this adapter. The bound artifact remains valid so already
// admitted callbacks can finish their generated/native transitions safely.
// Returns VM_ADAPTER_BUSY until hooks, generated-root leases/transitions,
// entered VM scopes/isolate ownership, and persistent object handles are idle.
// The call is idempotent and may be retried; it does not advance generation.
DARTPLANT_EXPORT DartPlantStatus
dartplant_flutter_vm_adapter_quiesce_artifacts(DartPlantFlutterVmAdapter* instance);
// Full unload barrier for the bound AOT artifact: first performs the quiescence
// step above; only after it reaches zero active work does it poison the binding
// and advance artifact_generation. DARTPLANT_OK means old AOT artifact code is
// no longer reachable through DartPlant hooks/transitions and may be unmapped.
// If libflutter/the isolate itself is being destroyed, release external object
// handles and destroy/detach this adapter while the engine mapping is still
// valid before unmapping libflutter.
DARTPLANT_EXPORT DartPlantStatus
dartplant_flutter_vm_adapter_retire_artifacts(DartPlantFlutterVmAdapter* instance);
// Revalidates the exact app/engine incarnation captured at create time. A new
// path, load bias, executable-range shape, or known Build ID is rejected and
// requires a new adapter instance; Build ID remains lifecycle identity only.
DARTPLANT_EXPORT DartPlantStatus
dartplant_flutter_vm_adapter_revalidate_artifacts(DartPlantFlutterVmAdapter* instance);
DARTPLANT_EXPORT DartPlantStatus
dartplant_flutter_vm_adapter_destroy(DartPlantFlutterVmAdapter* instance);
// Source-verified ABI descriptors compiled into this adapter. Snapshot identity
// ranks a finite candidate set; read-only structural and relational proofs
// select ABI domains. Artifact Build IDs never select private VM layouts. The
// implementation remains process-global because dart_api_dl itself is
// process-global; create returns VM_ADAPTER_BUSY while another instance is
// attached.
DARTPLANT_EXPORT uint32_t dartplant_flutter_vm_descriptor_count(void);
DARTPLANT_EXPORT const DartPlantFlutterVmDescriptor* dartplant_flutter_vm_descriptor_at(
    uint32_t index);

#ifdef __cplusplus
}
#endif

#endif
