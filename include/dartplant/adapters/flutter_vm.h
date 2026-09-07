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
    DARTPLANT_FLUTTER_VM_CAP_RUNTIME_ROOTS_PROVEN = 1u << 0,
    DARTPLANT_FLUTTER_VM_CAP_OWNER_IDENTITY_PROVEN = 1u << 1,
    DARTPLANT_FLUTTER_VM_CAP_CANONICAL_NULL_PROVEN = 1u << 2,
    DARTPLANT_FLUTTER_VM_CAP_REGISTER_SEMANTICS_PROVEN = 1u << 3,
    DARTPLANT_FLUTTER_VM_CAP_DART_CORE_PROVEN = 1u << 4,
    DARTPLANT_FLUTTER_VM_CAP_SAFEPOINT_STUBS_PROVEN = 1u << 5,
    DARTPLANT_FLUTTER_VM_CAP_GENERATED_TRANSITION_SOURCE_VERIFIED = 1u << 6,
    DARTPLANT_FLUTTER_VM_CAP_EXCEPTION_SOURCE_VERIFIED = 1u << 7,
    DARTPLANT_FLUTTER_VM_CAP_TYPE_ARGUMENTS_SOURCE_VERIFIED = 1u << 8,
    DARTPLANT_FLUTTER_VM_CAP_ARTIFACT_LIFECYCLE_BOUND = 1u << 9,
} DartPlantFlutterVmCapability;

typedef enum DartPlantFlutterVmProofState {
    DARTPLANT_FLUTTER_VM_PROOF_UNSUPPORTED = 0,
    DARTPLANT_FLUTTER_VM_PROOF_UNVERIFIED = 1,
    DARTPLANT_FLUTTER_VM_PROOF_VERIFIED = 2,
    DARTPLANT_FLUTTER_VM_PROOF_FAILED_FOR_INCARNATION = 3,
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
    // Deprecated compatibility field. Build IDs no longer select the Dart VM
    // private ABI. The adapter observes the currently mapped engine/app Build
    // IDs only as artifact-incarnation diagnostics after structural proof.
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
// This never changes the selected ABI or captured artifact identity.
DARTPLANT_EXPORT void dartplant_flutter_vm_adapter_invalidate_artifacts(
    DartPlantFlutterVmAdapter* instance);
// Revalidates the exact app/engine incarnation captured at create time. A new
// path, load bias, executable-range shape, or known Build ID is rejected and
// requires a new adapter instance; Build ID remains lifecycle identity only.
DARTPLANT_EXPORT DartPlantStatus
dartplant_flutter_vm_adapter_revalidate_artifacts(DartPlantFlutterVmAdapter* instance);
DARTPLANT_EXPORT DartPlantStatus
dartplant_flutter_vm_adapter_destroy(DartPlantFlutterVmAdapter* instance);
// Source-verified ABI descriptors compiled into this adapter. Snapshot identity
// is only a ranking hint; every compatible finite source candidate is eligible
// for read-only structural proof. Artifact Build IDs never determine ABI
// compatibility. The implementation remains process-global because dart_api_dl
// itself is process-global; create returns VM_ADAPTER_BUSY while another
// instance is attached.
DARTPLANT_EXPORT uint32_t dartplant_flutter_vm_descriptor_count(void);
DARTPLANT_EXPORT const DartPlantFlutterVmDescriptor* dartplant_flutter_vm_descriptor_at(
    uint32_t index);

#ifdef __cplusplus
}
#endif

#endif
