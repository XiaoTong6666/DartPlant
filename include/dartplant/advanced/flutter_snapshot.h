#ifndef DARTPLANT_ADVANCED_FLUTTER_SNAPSHOT_H_
#define DARTPLANT_ADVANCED_FLUTTER_SNAPSHOT_H_

#include <stdint.h>

#include "dartplant/dartplant.h"

typedef struct DartPlantFlutterSnapshotInfo {
    uint32_t struct_size;
    const char* module_name;
    const char* module_path;
    const char* module_build_id;
    const char* snapshot_hash;
    const char* snapshot_features;
    const char* profile_name;
    uint64_t load_bias;
    uint64_t isolate_instructions_va;
    uint64_t isolate_instructions_size;
    uint64_t isolate_instructions_runtime;
    uint8_t compressed_pointers;
} DartPlantFlutterSnapshotInfo;

// Compiler-side proof about the logical identity multiplicity of one physical
// Dart Code entry. UNKNOWN is deliberately not equivalent to UNIQUE: an
// artifact index that only contains one logical record cannot by itself prove
// that the precompiler did not deduplicate another Function onto the same Code.
typedef enum DartPlantCodeIdentityProof {
    DARTPLANT_CODE_IDENTITY_UNKNOWN = 0,
    DARTPLANT_CODE_IDENTITY_UNIQUE,
    DARTPLANT_CODE_IDENTITY_SHARED,
} DartPlantCodeIdentityProof;

typedef struct DartPlantSnapshotFunctionInfo {
    uint32_t struct_size;
    const char* library_uri;
    const char* class_name;
    const char* function_name;
    const char* signature;
    DartPlantEntryKind entry_kind;
    uint64_t entry_va;
    uint64_t code_size;
    uint64_t code_section_va;
    const char* fingerprint;
    DartPlantCodeIdentityProof code_identity_proof;
    uint32_t physical_entry_alias_count;
    // UntaggedFunction::Kind from the exact compiler artifact. The first three
    // stable SDK values are RegularFunction=0, ClosureFunction=1 and
    // ImplicitClosureFunction=2. closure_call_entry_only is asserted only when
    // compiler/source evidence proves PRODUCT AOT invokes it through the
    // cached Closure.entry_point (the Function normal entry).
    uint32_t function_kind;
    uint8_t closure_call_entry_only;
    uint8_t reserved_function_flags[3];
    // V3 append-only Code-payload identity. entry_va/code_size describe the
    // selected entry suffix, while these fields identify the complete Dart
    // Code payload shared by sibling entry kinds. Old artifacts may leave both
    // zero and are conservatively treated as one-entry payloads.
    uint64_t code_payload_va;
    uint64_t code_instructions_length;
} DartPlantSnapshotFunctionInfo;

typedef struct DartPlantSnapshotIndexInfo {
    uint32_t struct_size;
    const char* module_name;
    const char* module_build_id;
    const char* snapshot_hash;
    const char* dart_version;
    const char* profile_version;
    const DartPlantSnapshotFunctionInfo* functions;
    uint32_t function_count;
} DartPlantSnapshotIndexInfo;

#endif  // DARTPLANT_ADVANCED_FLUTTER_SNAPSHOT_H_
