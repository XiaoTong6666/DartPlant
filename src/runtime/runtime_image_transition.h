#ifndef DARTPLANT_RUNTIME_RUNTIME_IMAGE_TRANSITION_H_
#define DARTPLANT_RUNTIME_RUNTIME_IMAGE_TRANSITION_H_

#include <vector>

#include "core/internal.h"
#include "runtime/runtime_image_set.h"

struct DartPlantRuntime;

namespace dartplant {

// Stages and publishes one RuntimeImageSet ownership transition. The caller
// has already selected the target engine/isolate-group epochs and runtime
// generation on staged_images. This transaction owns the ordering boundary:
//
//   close image admission -> drain/retire hooks -> invalidate registries
//   -> retire old image owners -> activate/publish staged images
//
// A failed drain never publishes staged_images. Affected old images remain in
// DRAINING so stale methods cannot regain admission after the failure.
DartPlantStatus TransitionRuntimeImages(DartPlantRuntime* runtime, RuntimeImageSet staged_images,
                                        const std::vector<ModuleImage>& current_modules,
                                        bool invalidate_entire_runtime);

}  // namespace dartplant

#endif  // DARTPLANT_RUNTIME_RUNTIME_IMAGE_TRANSITION_H_
