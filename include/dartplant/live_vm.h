// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_LIVE_VM_H_
#define DARTPLANT_LIVE_VM_H_

#include <stdint.h>

#include "dartplant/dartplant.h"
#include "dartplant/flutter_snapshot.h"
#include "dartplant/invocation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DARTPLANT_LIVE_VM_FUNCTION_NAME_MAX 160
#define DARTPLANT_LIVE_VM_CLASS_NAME_MAX 160
#define DARTPLANT_LIVE_VM_LIBRARY_URI_MAX 320

typedef struct DartPlantLiveVmProfile {
    uint32_t struct_size;
    uint32_t profile_version;
    const char* name;
    const char* dart_version;
    const char* snapshot_hash;
    const char* snapshot_profile;

    uint8_t thr_register;
    uint8_t pp_register;
    uint8_t code_register;
    uint8_t heap_bits_register;
    uint8_t null_register;
    uint8_t reserved_registers[3];

    uint32_t thread_heap_base_offset;
    uint32_t thread_object_null_offset;
    uint32_t thread_global_object_pool_offset;
    uint32_t thread_isolate_offset;
    uint32_t thread_isolate_group_offset;

    uint32_t isolate_group_class_table_offset;
    uint32_t isolate_group_cached_class_table_table_offset;
    uint32_t isolate_group_object_store_offset;
    uint32_t class_table_num_cids_offset;
    uint32_t object_store_libraries_offset;

    uint32_t code_entry_point_offset;
    uint32_t code_object_pool_offset;
    uint32_t code_owner_offset;
    uint32_t code_instructions_length_offset;

    uint32_t function_entry_point_offset;
    uint32_t function_name_offset;
    uint32_t function_owner_offset;
    uint32_t function_code_offset;
    uint32_t function_kind_tag_offset;

    uint32_t class_name_offset;
    uint32_t class_functions_offset;
    uint32_t class_library_offset;

    uint32_t library_url_offset;
    uint32_t library_toplevel_class_offset;

    uint32_t array_length_offset;
    uint32_t array_elements_offset;
    uint32_t growable_object_array_length_offset;
    uint32_t growable_object_array_data_offset;
    uint32_t string_length_offset;
    uint32_t string_data_offset;
    uint32_t object_pool_length_offset;
    uint32_t object_pool_elements_offset;

    uint32_t cid_class;
    uint32_t cid_function;
    uint32_t cid_library;
    uint32_t cid_code;
    uint32_t cid_object_pool;
    uint32_t cid_array;
    uint32_t cid_immutable_array;
    uint32_t cid_growable_object_array;
    uint32_t cid_one_byte_string;
    uint32_t cid_two_byte_string;
} DartPlantLiveVmProfile;

typedef struct DartPlantLiveVmArm64Registers {
    uint32_t struct_size;
    uint32_t tid;
    uint64_t pc;
    uint64_t sp;
    uint64_t thr;
    uint64_t pp;
    uint64_t heap_bits;
    uint64_t null_value;
} DartPlantLiveVmArm64Registers;

typedef struct DartPlantLiveVmBootstrapOptions {
    uint32_t struct_size;
    uint32_t max_rounds;
    uint32_t per_thread_timeout_us;
    uint32_t round_sleep_us;
} DartPlantLiveVmBootstrapOptions;

typedef struct DartPlantLiveVmBootstrapInfo {
    uint32_t struct_size;
    uint32_t selected_tid;
    uint32_t signal_number;
    uint32_t rounds;
    uint32_t sampled_threads;
    uint32_t captured_contexts;
    uint32_t validated_candidates;
    uint32_t dart_instruction_samples;
    uint32_t nonzero_thr_samples;
    uint32_t last_candidate_tid;
    uint32_t signal_send_failures;
    uint32_t sample_timeouts;
    uint32_t ui_activity_polls;
    uint32_t ui_activity_hits;
    uint64_t selected_pc;
    uint64_t last_candidate_pc;
    uint64_t last_candidate_thr;
    uint64_t last_candidate_pp;
    uint64_t last_candidate_heap_bits;
    uint64_t last_candidate_null;
    char last_validation_error[192];
} DartPlantLiveVmBootstrapInfo;

typedef struct DartPlantLiveVmContext {
    uint32_t struct_size;
    uint32_t profile_version;
    const char* profile_name;
    uint32_t reserved;

    uint64_t thread;
    uint64_t isolate;
    uint64_t isolate_group;
    uint64_t class_table;
    uint64_t cached_class_table_table;
    uint64_t object_store;
    uint64_t heap_base;
    uint64_t pp;
    uint64_t global_object_pool;
    uint64_t object_pool_length;
} DartPlantLiveVmContext;

typedef enum DartPlantObjectPoolEntryType {
    DARTPLANT_OBJECT_POOL_IMMEDIATE = 0,
    DARTPLANT_OBJECT_POOL_TAGGED_OBJECT = 1,
    DARTPLANT_OBJECT_POOL_NATIVE_FUNCTION = 2,
    DARTPLANT_OBJECT_POOL_UNKNOWN = 255,
} DartPlantObjectPoolEntryType;

typedef struct DartPlantObjectPoolEntryInfo {
    uint32_t struct_size;
    uint32_t index;
    DartPlantObjectPoolEntryType type;
    // Boolean view of Dart ObjectPool::PatchableAt(). The encoded VM bit has
    // the opposite polarity: kPatchable=0, kNotPatchable=1.
    uint8_t patchable;
    uint8_t snapshot_behavior;
    uint8_t entry_bits;
    uint8_t reserved;
    uint64_t pool;
    uint64_t raw_value;
    uint64_t tagged_object;
    uint32_t object_cid;
    uint32_t reserved_cid;
    // OffsetFromIndex(index): byte offset from the tagged ObjectPoolPtr, not
    // from the untagged object address and not from PP.
    uint64_t byte_offset;
} DartPlantObjectPoolEntryInfo;

typedef struct DartPlantLiveVmFunctionInfo {
    uint32_t struct_size;
    uint32_t entry_alias_count;

    uint64_t function;
    uint64_t code;
    uint64_t code_object_pool;
    uint64_t function_entry_point;
    uint64_t code_entry_point;
    uint64_t entry_va;
    uint64_t code_section_va;
    uint32_t code_size;
    uint32_t function_kind;
    uint64_t owner_class;
    uint64_t library;

    uint8_t owner_is_toplevel_class;
    uint8_t entry_is_shared;
    uint8_t code_owner_matches_function;
    uint8_t reserved_flags[5];

    char function_name[DARTPLANT_LIVE_VM_FUNCTION_NAME_MAX];
    char class_name[DARTPLANT_LIVE_VM_CLASS_NAME_MAX];
    char library_uri[DARTPLANT_LIVE_VM_LIBRARY_URI_MAX];
} DartPlantLiveVmFunctionInfo;

typedef struct DartPlantLiveVmFunctionIndexInfo {
    uint32_t struct_size;
    uint32_t function_count;
    uint32_t code_target_count;
    uint32_t shared_code_target_count;
    uint32_t skipped_function_count;
    uint32_t reserved;
} DartPlantLiveVmFunctionIndexInfo;

typedef uint8_t (*DartPlantLiveVmFunctionVisitor)(const DartPlantLiveVmFunctionInfo* function,
                                                  void* user_data);

typedef struct DartPlantLiveVmMethodInfo {
    uint32_t struct_size;
    uint32_t entry_alias_count;

    uint64_t function;
    uint64_t function_entry_point;
    uint64_t code;
    uint64_t code_entry_point;
    uint64_t code_owner;
    uint32_t code_size;
    uint32_t reserved_code_size;
    uint64_t owner_class;
    uint64_t library;

    uint8_t owner_is_toplevel_class;
    uint8_t entry_is_shared;
    uint8_t function_in_class_functions;
    uint8_t function_code_owner_match;
    uint8_t code_owner_is_function;
    uint8_t code_owner_mismatch_allowed;
    uint8_t code_entry_matches_function;
    uint8_t reserved_flags;

    char function_name[DARTPLANT_LIVE_VM_FUNCTION_NAME_MAX];
    char class_name[DARTPLANT_LIVE_VM_CLASS_NAME_MAX];
    char library_uri[DARTPLANT_LIVE_VM_LIBRARY_URI_MAX];
} DartPlantLiveVmMethodInfo;

typedef struct DartPlantLiveVmProbeInfo {
    uint32_t struct_size;
    uint32_t profile_version;
    const char* profile_name;
    uint32_t entry_alias_count;
    uint32_t reserved_count;

    uint64_t thread;
    uint64_t isolate;
    uint64_t isolate_group;
    uint64_t class_table;
    uint64_t cached_class_table_table;
    uint64_t object_store;
    uint64_t heap_base;

    uint64_t pp;
    uint64_t global_object_pool;
    uint64_t object_pool_length;
    uint64_t code_register;
    uint64_t code;
    uint64_t code_entry_point;
    uint64_t code_owner;
    uint64_t function;
    uint64_t function_entry_point;
    uint64_t owner_class;
    uint64_t library;

    uint8_t heap_bits_match;
    uint8_t null_register_match;
    uint8_t thread_pool_match;
    uint8_t code_pool_match;
    uint8_t code_pool_is_null;
    uint8_t function_code_match;
    uint8_t code_owner_is_function;
    uint8_t code_owner_mismatch_allowed;
    uint8_t code_entry_matches_function;
    uint8_t function_in_class_functions;
    uint8_t owner_is_toplevel_class;
    uint8_t function_found_from_vm_index;
    uint8_t entry_is_shared;
    uint8_t reserved_flags[4];

    char function_name[DARTPLANT_LIVE_VM_FUNCTION_NAME_MAX];
    char class_name[DARTPLANT_LIVE_VM_CLASS_NAME_MAX];
    char library_uri[DARTPLANT_LIVE_VM_LIBRARY_URI_MAX];
} DartPlantLiveVmProbeInfo;

// Selects a data-only layout profile from the exact snapshot identity. Unknown
// snapshots fail closed instead of guessing private VM object offsets.
DARTPLANT_EXPORT DartPlantStatus dartplant_live_vm_select_profile(
    const DartPlantFlutterSnapshotInfo* snapshot, DartPlantLiveVmProfile* out_profile);

// Validates an ARM64 register sample and reconstructs reusable Dart VM roots.
// This path does not require a known Dart method address and is shared by the
// cold-bootstrap sampler and invocation diagnostics.
DARTPLANT_EXPORT DartPlantStatus dartplant_live_vm_context_from_arm64_registers(
    const DartPlantFlutterSnapshotInfo* snapshot, const DartPlantLiveVmArm64Registers* registers,
    DartPlantLiveVmContext* out_context);

// Samples process threads on Android/Linux ARM64 and accepts a candidate only
// after the full THR/PP/HEAP_BITS/NULL/ObjectPool/ClassTable/ObjectStore
// semantic validation succeeds. The signal handler only copies ucontext
// registers; VM memory is inspected after returning to normal execution.
DARTPLANT_EXPORT DartPlantStatus dartplant_live_vm_bootstrap_process(
    const DartPlantFlutterSnapshotInfo* snapshot, const DartPlantLiveVmBootstrapOptions* options,
    DartPlantLiveVmContext* out_context, DartPlantLiveVmBootstrapInfo* out_info);

// Reads the current Dart execution context without calling Dart private C++
// symbols. The probe validates THR/PP/HEAP_BITS, records CODE_REG for diagnosis,
// and reconstructs the current Function -> Code -> Class -> Library chain from
// raw target-VM memory.
DARTPLANT_EXPORT DartPlantStatus dartplant_live_vm_probe_invocation(
    const DartPlantInvocation* invocation, const DartPlantFlutterSnapshotInfo* snapshot,
    DartPlantLiveVmProbeInfo* out_info);

// Detaches the stable VM roots discovered by a successful invocation probe from
// the callback lifetime. The resulting context contains raw target-VM roots and
// can be reused for identity lookups while the isolate group remains alive.
DARTPLANT_EXPORT DartPlantStatus dartplant_live_vm_context_from_probe(
    const DartPlantLiveVmProbeInfo* probe, DartPlantLiveVmContext* out_context);

// Resolves a Dart method directly from the live target VM by semantic identity.
// This lookup does not consume metadata and does not require the method to be
// hooked already; metadata may still be used separately as a test oracle.
DARTPLANT_EXPORT DartPlantStatus dartplant_live_vm_find_method(
    const DartPlantLiveVmContext* context, const DartPlantFlutterSnapshotInfo* snapshot,
    const char* library_uri, const char* class_name, const char* function_name,
    DartPlantLiveVmMethodInfo* out_method);

// Enumerates the runtime Function graph reconstructed from Class.functions and
// Library.toplevel_class. This is the production replacement for precomputed
// method metadata: every emitted record carries the live Function*, Code*,
// runtime entry and ELF entry_va. Closures are intentionally skipped.
DARTPLANT_EXPORT DartPlantStatus dartplant_live_vm_visit_functions(
    const DartPlantLiveVmContext* context, const DartPlantFlutterSnapshotInfo* snapshot,
    DartPlantLiveVmFunctionVisitor visitor, void* user_data,
    DartPlantLiveVmFunctionIndexInfo* out_info);

// Converts between Dart ObjectPool indexes and offsets exactly as
// ObjectPool::OffsetFromIndex()/IndexFromOffset() do. Offsets are relative to
// the tagged ObjectPoolPtr, hence the heap-object-tag subtraction.
DARTPLANT_EXPORT DartPlantStatus dartplant_live_vm_object_pool_offset_from_index(
    const DartPlantFlutterSnapshotInfo* snapshot, uint32_t index, uint64_t* out_offset);
DARTPLANT_EXPORT DartPlantStatus dartplant_live_vm_object_pool_index_from_offset(
    const DartPlantFlutterSnapshotInfo* snapshot, uint64_t offset, uint32_t* out_index);

// Decodes one ObjectPool slot using the VM's runtime index space. The data word
// lives at elements_start + index * word_size; its entry_bits byte follows the
// complete data array. Entry type and patchability match Dart ObjectPool::TypeAt
// and PatchableAt semantics.
DARTPLANT_EXPORT DartPlantStatus dartplant_live_vm_read_object_pool_entry(
    const DartPlantLiveVmContext* context, const DartPlantFlutterSnapshotInfo* snapshot,
    uint64_t tagged_object_pool, uint32_t index, DartPlantObjectPoolEntryInfo* out_entry);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_LIVE_VM_H_
