#ifndef DARTPLANT_ADVANCED_FLUTTER_SNAPSHOT_H_
#define DARTPLANT_ADVANCED_FLUTTER_SNAPSHOT_H_

#include <stdint.h>

#include "dartplant/dartplant.h"

struct DartPlantFlutterSnapshotInfo {
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
};

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
