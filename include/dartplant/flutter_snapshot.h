#ifndef DARTPLANT_FLUTTER_SNAPSHOT_H_
#define DARTPLANT_FLUTTER_SNAPSHOT_H_

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

#endif  // DARTPLANT_FLUTTER_SNAPSHOT_H_
