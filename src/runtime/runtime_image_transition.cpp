#include "runtime/runtime_image_transition.h"

#include <algorithm>

#include "runtime/runtime_internal.h"

namespace dartplant {
namespace {

bool SameExecutableRangeForTransition(const ExecutableRange& left, const ExecutableRange& right) {
    return left.start == right.start && left.end == right.end &&
           left.file_offset == right.file_offset && left.virtual_address == right.virtual_address &&
           left.file_size == right.file_size;
}

bool SameModuleForTransition(const ModuleImage& left, const ModuleImage& right) {
    return left.name == right.name && left.path == right.path && left.build_id == right.build_id &&
           left.load_bias == right.load_bias &&
           left.executable_ranges.size() == right.executable_ranges.size() &&
           std::equal(left.executable_ranges.begin(), left.executable_ranges.end(),
                      right.executable_ranges.begin(), SameExecutableRangeForTransition);
}

bool MappingStillPresent(const std::vector<ModuleImage>& modules, const RuntimeImage& image) {
    return std::any_of(modules.begin(), modules.end(), [&image](const ModuleImage& module) {
        return SameModuleForTransition(module, image.module);
    });
}

bool SamePublishedOwner(const RuntimeImage& left, const RuntimeImage& right) {
    return left.id == right.id && left.incarnation_epoch == right.incarnation_epoch &&
           left.runtime_generation == right.runtime_generation &&
           left.engine_incarnation_epoch == right.engine_incarnation_epoch &&
           left.isolate_group_incarnation_epoch == right.isolate_group_incarnation_epoch;
}

}  // namespace

DartPlantStatus TransitionRuntimeImages(DartPlantRuntime* runtime, RuntimeImageSet staged_images,
                                        const std::vector<ModuleImage>& current_modules,
                                        bool invalidate_entire_runtime) {
    if (runtime == nullptr) return DARTPLANT_INVALID_ARGUMENT;
    auto& group = RuntimeIsolateGroup(runtime);

    std::vector<RuntimeImage> affected;
    affected.reserve(group.image_set.size());
    for (const RuntimeImage& old_image : group.image_set.images()) {
        const RuntimeImage* replacement = staged_images.FindById(old_image.id);
        if (!invalidate_entire_runtime && replacement != nullptr &&
            SamePublishedOwner(old_image, *replacement)) {
            continue;
        }
        affected.push_back(old_image);
    }

    // Close logical admission before touching physical hooks. Public method
    // lookup holds runtime->mutex around this transaction, while already
    // admitted callbacks are pinned by HookRecord::in_flight and the published
    // host gate. This is the image-level equivalent of the hook DRAINING gate.
    for (const RuntimeImage& image : affected) {
        if (!group.image_set.BeginDrain(image.id, image.incarnation_epoch)) {
            SetLastError("runtime image transition could not close old image admission");
            return DARTPLANT_RUNTIME_NOT_READY;
        }
    }

    DartPlantStatus drain_status = DARTPLANT_OK;
    if (invalidate_entire_runtime) {
        const bool every_mapping_gone =
            !affected.empty() &&
            std::all_of(affected.begin(), affected.end(),
                        [&current_modules](const RuntimeImage& image) {
                            return !MappingStillPresent(current_modules, image);
                        });
        if (every_mapping_gone) {
            RetireRuntimeHooks(runtime->generation);
        } else {
            drain_status = InvalidateRuntimeHooks(runtime->generation);
        }
    } else {
        for (const RuntimeImage& image : affected) {
            if (MappingStillPresent(current_modules, image)) {
                const DartPlantStatus status = InvalidateRuntimeImageHooks(
                    runtime->generation, image.id, image.incarnation_epoch);
                if (drain_status == DARTPLANT_OK && status != DARTPLANT_OK) {
                    drain_status = status;
                }
            } else {
                RetireRuntimeImageHooks(runtime->generation, image.id, image.incarnation_epoch);
            }
        }
    }
    if (drain_status != DARTPLANT_OK) {
        // Deliberately leave affected owners in DRAINING and retain their
        // registries/stubs. Publishing the staged set here would recreate the
        // exact remap race that owner epochs are intended to prevent.
        return drain_status;
    }

    if (invalidate_entire_runtime) {
        group.entry_targets.Clear();
        group.abi_evidence.clear();
    } else {
        for (const RuntimeImage& image : affected) {
            group.entry_targets.EraseImage(image.id, image.incarnation_epoch);
            EraseRuntimeAbiEvidenceForImage(runtime, image.id, image.incarnation_epoch);
        }
    }
    for (const RuntimeImage& image : affected) {
        (void) group.image_set.Retire(image.id, image.incarnation_epoch);
    }

    staged_images.ActivateAll();
    group.image_set = std::move(staged_images);
    group.live_snapshot_index.reset();
    group.live_function_index_info = {};
    ClearLastError();
    return DARTPLANT_OK;
}

}  // namespace dartplant
