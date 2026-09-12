// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_RUNTIME_H_
#define DARTPLANT_RUNTIME_H_

// Advanced explicit-runtime API. Normal consumers should prefer
// dartplant_init() + dartplant_find_method() + dartplant_hook_method(). This
// header remains public for diagnostics, fixtures, custom bootstrap policy and
// source compatibility with earlier DartPlant releases.

#include <stdint.h>

#include "dartplant/advanced/abi_evidence.h"
#include "dartplant/advanced/diagnostics.h"
#include "dartplant/advanced/flutter_snapshot.h"
#include "dartplant/advanced/live_vm.h"
#include "dartplant/advanced/runtime_profile.h"
#include "dartplant/dartplant.h"
#include "dartplant/invocation.h"
#include "dartplant/signature.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DartPlantRuntime DartPlantRuntime;

typedef enum DartPlantRuntimeState {
    DARTPLANT_RUNTIME_CREATED = 0,
    DARTPLANT_RUNTIME_IMAGES_READY,
    DARTPLANT_RUNTIME_READY,
    DARTPLANT_RUNTIME_FAILED,
} DartPlantRuntimeState;

typedef struct DartPlantRuntimeInfo {
    uint32_t struct_size;
    DartPlantRuntimeState state;
    uint32_t loaded_module_count;
    uint8_t app_module_loaded;
    uint8_t runtime_module_loaded;
    uint8_t live_function_index_ready;
    uint8_t profile_matched;
} DartPlantRuntimeInfo;

typedef enum DartPlantRuntimeImageKind {
    DARTPLANT_RUNTIME_IMAGE_ROOT = 0,
    DARTPLANT_RUNTIME_IMAGE_DEFERRED,
} DartPlantRuntimeImageKind;

typedef struct DartPlantRuntimeImageInfo {
    uint32_t struct_size;
    DartPlantRuntimeImageKind kind;
    uint64_t image_id;
    uint64_t runtime_generation;
    uint32_t loading_unit_id;
    uint32_t reserved;
    const char* module_name;
    const char* module_path;
    const char* module_build_id;
    const char* snapshot_hash;
    const char* snapshot_features;
    const char* profile_name;
    uint64_t isolate_instructions_va;
    uint64_t isolate_instructions_size;
    uint64_t isolate_instructions_runtime;
    // Retained live Function entry families whose Code payloads uniquely
    // resolve into this image for the current isolate-group generation. A
    // non-zero count means the physical image has semantic live-VM ownership
    // evidence, not merely filename/module/snapshot provenance.
    uint32_t live_entry_count;
    uint8_t live_semantic_bound;
    uint8_t has_deferred_program_hash;
    uint8_t deferred_program_hash_vm_bound;
    uint8_t reserved_binding;
    uint32_t deferred_program_hash;
} DartPlantRuntimeImageInfo;

DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_create(const DartPlantRuntimeProfile* profile,
                                                          DartPlantRuntime** out_runtime);
DARTPLANT_EXPORT void dartplant_runtime_destroy(DartPlantRuntime* runtime);

// Refreshes the complete ELF image list. module_name/module_handle describe the
// host event that triggered the refresh; the handle is never treated as a load
// bias or function address.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_on_module_loaded(DartPlantRuntime* runtime,
                                                                    const char* module_name,
                                                                    void* module_handle);
// Re-enumerates the complete process image set. Hosts should call the unload
// form before dlclose when possible so active hooks are removed while mappings
// remain valid. Call refresh_modules after the loader operation; replacement
// incarnations advance runtime generation and require a new live-VM bootstrap.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_on_module_unloading(DartPlantRuntime* runtime,
                                                                       const char* module_name,
                                                                       void* module_handle);
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_refresh_modules(DartPlantRuntime* runtime);

// Binds this runtime to the unique Flutter engine mapping containing one
// engine-owned executable address (for example an API-DL function entry). This
// is optional for the ordinary single-engine case and is the fail-closed
// disambiguator when multiple matching libflutter.so incarnations coexist.
// Passing nullptr clears the anchor and re-runs module selection.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_bind_engine_anchor(DartPlantRuntime* runtime,
                                                                      const void* engine_address);

DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_get_info(const DartPlantRuntime* runtime,
                                                            DartPlantRuntimeInfo* out_info);

// Enumerates the source-proven Dart AOT instruction namespaces owned by the
// current app incarnation. Index zero is not semantically special; callers
// should use kind/loading_unit_id/image_id. image_id remains stable only for
// the current runtime generation.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_get_image_count(const DartPlantRuntime* runtime,
                                                                   uint32_t* out_count);
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_get_image_info(
    const DartPlantRuntime* runtime, uint32_t index, DartPlantRuntimeImageInfo* out_info);

// Returns the latest structured proof/rejection snapshot. This is diagnostic
// state only: callers must still use the operation's DartPlantStatus as the
// authoritative result.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_get_resolution_diagnostics(
    const DartPlantRuntime* runtime, DartPlantResolutionDiagnostics* out_diagnostics);

DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_get_flutter_snapshot(
    const DartPlantRuntime* runtime, DartPlantFlutterSnapshotInfo* out_info);

// Captures a validated live-VM context from an active Dart invocation. Once
// captured, exact default-entry method queries can be resolved directly from
// the target VM without consulting metadata for their address or identity.
DARTPLANT_EXPORT DartPlantStatus
dartplant_runtime_capture_live_vm(DartPlantRuntime* runtime, const DartPlantInvocation* invocation);

// Performs process-wide discovery of an active Dart mutator context without
// external metadata or a precomputed function index. This is valid once the
// Flutter app/runtime images and snapshot are discovered; a successful
// bootstrap promotes IMAGES_READY to READY.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_bootstrap_live_vm(
    DartPlantRuntime* runtime, const DartPlantLiveVmBootstrapOptions* options,
    DartPlantLiveVmBootstrapInfo* out_info);

// Fast path for hosts that already execute on a Dart mutator thread (for
// example a Dart FFI entry). The supplied ARM64 Dart-reserved registers still
// pass the same full semantic validation as sampled contexts. No metadata is
// consulted. A successful call promotes IMAGES_READY to READY.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_bootstrap_live_vm_from_arm64_registers(
    DartPlantRuntime* runtime, const DartPlantLiveVmArm64Registers* registers,
    DartPlantLiveVmBootstrapInfo* out_info);

// Returns the automatically generated runtime Function index. The index is
// rebuilt from live Class.functions/Library.toplevel_class when a LiveVmContext
// is captured or bootstrapped; no metadata or offline SnapshotIndex is required.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_get_function_index_info(
    const DartPlantRuntime* runtime, DartPlantLiveVmFunctionIndexInfo* out_info);
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_get_function_info(
    const DartPlantRuntime* runtime, uint32_t index, DartPlantLiveVmFunctionInfo* out_info);

// Registers an exact, build-bound snapshot index for logical Functions that the
// Dart AOT precompiler removed from the PRODUCT heap. Retained Function objects
// are still resolved from the live VM first; this index is consulted only after
// an exact live-index miss. The index must match the current snapshot hash and
// libapp.so build-id and every record must carry a code fingerprint. An index is
// immutable for the lifetime of the current app/snapshot incarnation.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_register_snapshot_index(
    DartPlantRuntime* runtime, const DartPlantSnapshotIndexInfo* index);

// Reads the retained Dart FunctionType for a live runtime method. A stale method
// or an AOT-dropped Function.signature fails closed; no ABI is inferred here.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_get_method_signature(
    const DartPlantRuntime* runtime, const DartPlantMethod* method,
    DartPlantDartFunctionSignatureInfo* out_signature);
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_get_method_parameter(
    const DartPlantRuntime* runtime, const DartPlantMethod* method, uint32_t index,
    DartPlantDartParameterInfo* out_parameter);

DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_read_global_object_pool_entry(
    const DartPlantRuntime* runtime, uint32_t index, DartPlantObjectPoolEntryInfo* out_entry);

// Resolves supported runtime identities only from the automatically built live
// Function index first. If an exact snapshot index was explicitly registered,
// Functions dropped from the PRODUCT heap may fall back to that artifact-bound
// index after a live-index miss.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_find_method(DartPlantRuntime* runtime,
                                                               const DartPlantMethodQuery* query,
                                                               DartPlantMethod** out_method);

// Registers exact compiler-side ABI evidence for the current live method
// incarnation. DartPlant binds the evidence to the method identity, physical
// entry target and runtime generation before deriving a DartCallLayout. Evidence
// cannot be installed after that physical target is already hooked.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_register_compiler_abi_evidence(
    DartPlantRuntime* runtime, const DartPlantMethod* method,
    const DartPlantCompilerAbiEvidence* evidence);
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_get_method_abi_info(
    const DartPlantRuntime* runtime, const DartPlantMethod* method,
    DartPlantMethodAbiInfo* out_info);

// Raw replacement path. The replacement must exactly match the target AOT
// entry ABI. Typed Dart invocation callbacks are only enabled by a validated
// profile and a dispatcher implementation.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_hook_method_raw(DartPlantRuntime* runtime,
                                                                   const DartPlantMethod* method,
                                                                   void* replacement, void** backup,
                                                                   DartPlantHook** out_hook);

DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_hook_method(DartPlantRuntime* runtime,
                                                               const DartPlantMethod* method,
                                                               const DartPlantHookOptions* options,
                                                               DartPlantHook** out_hook);

// Transitional advanced entry returning the same logical handle as the normal
// dartplant_hook_method() facade while an explicitly managed runtime is still
// required by diagnostics/fixtures.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_hook_method_handle(
    DartPlantRuntime* runtime, const DartPlantMethod* method, const DartPlantHookOptions* options,
    DartPlantHookHandle** out_handle);

DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_hook_method_with_profile(
    DartPlantRuntime* runtime, const DartPlantMethod* method,
    const DartPlantRuntimeProfile* profile, const DartPlantHookOptions* options,
    DartPlantHook** out_hook);

// Adds a listener to an existing method hook. Higher priority listeners run
// first on enter and last on leave. Equal priorities preserve registration
// order. The returned listener can be removed independently of the underlying
// trampoline.
DARTPLANT_EXPORT DartPlantStatus dartplant_runtime_add_listener(DartPlantRuntime* runtime,
                                                                const DartPlantMethod* method,
                                                                const DartPlantHookOptions* options,
                                                                int32_t priority,
                                                                DartPlantListener** out_listener);
DARTPLANT_EXPORT DartPlantStatus dartplant_remove_listener(DartPlantListener* listener);
DARTPLANT_EXPORT void dartplant_release_listener(DartPlantListener* listener);
DARTPLANT_EXPORT uint8_t dartplant_listener_is_active(const DartPlantListener* listener);
// Returns true when no invocation snapshot still references this listener.
// Call remove first, wait for this to become true, then release the handle or
// its user_data.
DARTPLANT_EXPORT uint8_t dartplant_listener_is_idle(const DartPlantListener* listener);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_RUNTIME_H_
