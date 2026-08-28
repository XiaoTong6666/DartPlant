// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_ABI_EVIDENCE_SOLVER_H_
#define DARTPLANT_ABI_EVIDENCE_SOLVER_H_

#include <span>

#include "abi/evidence.h"

namespace dartplant::abi {

DartAbiResolvedSlot ResolveSlotEvidence(std::span<const DartAbiSlotEvidence> evidence);

// Combines exact providers for the same logical Function. Conflicting proven
// facts never degrade to Tagged: they remain explicitly conflicting so typed
// invocation can fail closed.
DartFunctionAbiResolution ResolveFunctionAbiEvidence(
    std::span<const DartFunctionAbiEvidence> providers, uint32_t parameter_count);

}  // namespace dartplant::abi

#endif  // DARTPLANT_ABI_EVIDENCE_SOLVER_H_
