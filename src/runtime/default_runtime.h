// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_RUNTIME_DEFAULT_RUNTIME_H_
#define DARTPLANT_RUNTIME_DEFAULT_RUNTIME_H_

#include "dartplant/dartplant.h"
#include "dartplant/hook.h"

struct DartPlantRuntime;

namespace dartplant {

bool DefaultRuntimeInitialized();
// Internal host-test visibility only; not part of the exported C ABI.
DartPlantRuntime* DefaultRuntimeInstanceForTesting();
DartPlantStatus FindDefaultRuntimeMethod(const DartPlantMethodQuery* query,
                                         DartPlantMethod** out_method);
DartPlantStatus BindRegisteredArtifactIndexIfReady(::DartPlantRuntime* runtime);
DartPlantStatus BindRegisteredCompilerEvidenceIfPresent(::DartPlantRuntime* runtime,
                                                        const DartPlantMethod* method);

}  // namespace dartplant

#endif  // DARTPLANT_RUNTIME_DEFAULT_RUNTIME_H_
