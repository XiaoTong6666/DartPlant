// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ADVANCED_ARTIFACT_H_
#define DARTPLANT_ADVANCED_ARTIFACT_H_

#include <stdint.h>

#include "dartplant/advanced/abi_evidence.h"
#include "dartplant/advanced/flutter_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { DARTPLANT_ARTIFACT_BUNDLE_VERSION = 1 };

// Compiler/tooling ingestion contract. Normal consumers only pass an opaque
// pointer to this bundle through DartPlantInitInfo; generated sidecar headers
// construct the concrete value.
typedef struct DartPlantArtifactBundle {
    uint32_t struct_size;
    uint32_t version;
    const DartPlantSnapshotIndexInfo* snapshot_index;
    const DartPlantCompilerAbiEvidence* compiler_abi_evidence;
    uint32_t compiler_abi_evidence_count;
} DartPlantArtifactBundle;

// Generated sidecars call this from a tiny C++ static registrar. Registration
// may happen before dartplant_init(); the normal runtime consumes the embedded
// bundle lazily when it is initialized and when the matching app image appears.
DARTPLANT_EXPORT DartPlantStatus
dartplant_register_embedded_artifact_bundle(const DartPlantArtifactBundle* bundle);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // DARTPLANT_ADVANCED_ARTIFACT_H_
