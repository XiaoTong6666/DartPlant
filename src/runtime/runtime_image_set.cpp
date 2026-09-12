// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "runtime/runtime_image_set.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>

namespace dartplant {
namespace {

bool SameExecutableRange(const ExecutableRange& left, const ExecutableRange& right) {
    return left.start == right.start && left.end == right.end &&
           left.file_offset == right.file_offset && left.virtual_address == right.virtual_address &&
           left.file_size == right.file_size;
}

bool SameModule(const ModuleImage& left, const ModuleImage& right) {
    return left.name == right.name && left.path == right.path && left.build_id == right.build_id &&
           left.load_bias == right.load_bias &&
           left.executable_ranges.size() == right.executable_ranges.size() &&
           std::equal(left.executable_ranges.begin(), left.executable_ranges.end(),
                      right.executable_ranges.begin(), SameExecutableRange);
}

bool SameSnapshot(const FlutterSnapshotSource& left, const FlutterSnapshotSource& right) {
    return left.module_name == right.module_name && left.module_path == right.module_path &&
           left.module_build_id == right.module_build_id &&
           left.snapshot_hash == right.snapshot_hash &&
           left.snapshot_features == right.snapshot_features &&
           left.profile_name == right.profile_name &&
           left.isolate_instructions_va == right.isolate_instructions_va &&
           left.isolate_instructions_size == right.isolate_instructions_size &&
           left.isolate_instructions_runtime == right.isolate_instructions_runtime &&
           left.compressed_pointers == right.compressed_pointers &&
           left.deferred_program_hash == right.deferred_program_hash;
}

bool SameImageIdentity(const RuntimeImage& left, const RuntimeImage& right) {
    return left.kind == right.kind && left.loading_unit_id == right.loading_unit_id &&
           SameModule(left.module, right.module) && SameSnapshot(left.snapshot, right.snapshot);
}

bool RangesOverlap(uintptr_t left_start, uint64_t left_size, uintptr_t right_start,
                   uint64_t right_size) {
    if (left_size == 0 || right_size == 0 || left_size > UINTPTR_MAX - left_start ||
        right_size > UINTPTR_MAX - right_start) {
        return true;
    }
    const uintptr_t left_end = left_start + static_cast<uintptr_t>(left_size);
    const uintptr_t right_end = right_start + static_cast<uintptr_t>(right_size);
    return left_start < right_end && right_start < left_end;
}

bool CompatibleWithRoot(const RuntimeImage& root, const FlutterSnapshotSource& snapshot) {
    // Snapshot hash identifies the Dart VM snapshot ABI family, while exact
    // feature/profile agreement prevents a secondary image from silently
    // switching PRODUCT/compressed-pointer semantics inside one isolate group.
    return snapshot.snapshot_hash == root.snapshot.snapshot_hash &&
           snapshot.snapshot_features == root.snapshot.snapshot_features &&
           snapshot.profile_name == root.snapshot.profile_name &&
           snapshot.compressed_pointers == root.snapshot.compressed_pointers;
}

std::optional<uint32_t> ParseDeferredSuffix(std::string_view prefix, std::string_view candidate) {
    if (!candidate.starts_with(prefix) || !candidate.ends_with(".part.so")) return std::nullopt;
    const size_t begin = prefix.size();
    const size_t end = candidate.size() - std::string_view(".part.so").size();
    if (begin >= end) return std::nullopt;
    uint32_t id = 0;
    const char* first = candidate.data() + begin;
    const char* last = candidate.data() + end;
    const auto parsed = std::from_chars(first, last, id);
    if (parsed.ec != std::errc{} || parsed.ptr != last || id <= 1) return std::nullopt;
    return id;
}

}  // namespace

bool RuntimeImage::ContainsRuntimeRange(uintptr_t address, size_t size) const {
    if (size == 0 || snapshot.isolate_instructions_runtime == 0 ||
        snapshot.isolate_instructions_size == 0 ||
        address < snapshot.isolate_instructions_runtime) {
        return false;
    }
    const uint64_t offset = address - snapshot.isolate_instructions_runtime;
    return offset < snapshot.isolate_instructions_size &&
           static_cast<uint64_t>(size) <= snapshot.isolate_instructions_size - offset &&
           module.ContainsExecutable(address, size);
}

std::optional<uint64_t> RuntimeImage::RuntimeAddressToElfVa(uintptr_t address, size_t size) const {
    if (!ContainsRuntimeRange(address, size)) return std::nullopt;
    const uint64_t offset = address - snapshot.isolate_instructions_runtime;
    if (snapshot.isolate_instructions_va > UINT64_MAX - offset) return std::nullopt;
    return snapshot.isolate_instructions_va + offset;
}

void RuntimeImageSet::Clear() {
    images_.clear();
    root_id_ = kInvalidRuntimeImageId;
    next_id_ = 1;
}

RuntimeImageId RuntimeImageSet::AllocateId() {
    if (next_id_ == kInvalidRuntimeImageId) return kInvalidRuntimeImageId;
    return next_id_++;
}

bool RuntimeImageSet::SetRoot(const ModuleImage& module, const FlutterSnapshotSource& snapshot,
                              uint64_t runtime_generation, std::string* error) {
    Clear();
    if (!snapshot.Matches(module) || snapshot.isolate_instructions_size == 0 ||
        snapshot.isolate_instructions_size > SIZE_MAX ||
        !module.ContainsExecutable(snapshot.isolate_instructions_runtime,
                                   static_cast<size_t>(snapshot.isolate_instructions_size))) {
        if (error != nullptr)
            *error = "root runtime image has inconsistent module/snapshot identity";
        return false;
    }
    RuntimeImage image;
    image.id = AllocateId();
    if (image.id == kInvalidRuntimeImageId) {
        if (error != nullptr) *error = "runtime image id space is exhausted";
        return false;
    }
    image.kind = RuntimeImageKind::kRoot;
    image.loading_unit_id = 1;
    image.runtime_generation = runtime_generation;
    image.module = module;
    image.snapshot = snapshot;
    root_id_ = image.id;
    images_.push_back(std::move(image));
    return true;
}

bool RuntimeImageSet::AddDeferred(const ModuleImage& module, const FlutterSnapshotSource& snapshot,
                                  uint32_t loading_unit_id, uint64_t runtime_generation,
                                  std::string* error) {
    const RuntimeImage* root = Root();
    if (root == nullptr || loading_unit_id <= 1 || !snapshot.Matches(module) ||
        snapshot.isolate_instructions_size == 0 || snapshot.isolate_instructions_size > SIZE_MAX ||
        !module.ContainsExecutable(snapshot.isolate_instructions_runtime,
                                   static_cast<size_t>(snapshot.isolate_instructions_size)) ||
        !CompatibleWithRoot(*root, snapshot) || !snapshot.deferred_program_hash.has_value()) {
        if (error != nullptr) *error = "deferred runtime image is incompatible with the root image";
        return false;
    }
    for (const auto& existing : images_) {
        if (existing.loading_unit_id == loading_unit_id) {
            if (error != nullptr) *error = "deferred loading-unit id is ambiguous";
            return false;
        }
        if (SameModule(existing.module, module)) {
            if (error != nullptr)
                *error = "one module incarnation cannot own multiple runtime images";
            return false;
        }
        if (RangesOverlap(existing.snapshot.isolate_instructions_runtime,
                          existing.snapshot.isolate_instructions_size,
                          snapshot.isolate_instructions_runtime,
                          snapshot.isolate_instructions_size)) {
            if (error != nullptr) *error = "runtime image instruction namespaces overlap";
            return false;
        }
        if (existing.kind == RuntimeImageKind::kDeferred &&
            existing.snapshot.deferred_program_hash != snapshot.deferred_program_hash) {
            if (error != nullptr) *error = "deferred loading units disagree on Dart program hash";
            return false;
        }
    }

    RuntimeImage image;
    image.id = AllocateId();
    if (image.id == kInvalidRuntimeImageId) {
        if (error != nullptr) *error = "runtime image id space is exhausted";
        return false;
    }
    image.kind = RuntimeImageKind::kDeferred;
    image.loading_unit_id = loading_unit_id;
    image.runtime_generation = runtime_generation;
    image.module = module;
    image.snapshot = snapshot;
    images_.push_back(std::move(image));
    return true;
}

const RuntimeImage* RuntimeImageSet::Root() const { return FindById(root_id_); }

const RuntimeImage* RuntimeImageSet::FindById(RuntimeImageId id) const {
    if (id == kInvalidRuntimeImageId) return nullptr;
    const auto found = std::find_if(images_.begin(), images_.end(),
                                    [id](const RuntimeImage& image) { return image.id == id; });
    return found == images_.end() ? nullptr : &*found;
}

const RuntimeImage* RuntimeImageSet::FindByLoadingUnitId(uint32_t loading_unit_id) const {
    const auto found =
        std::find_if(images_.begin(), images_.end(), [loading_unit_id](const RuntimeImage& image) {
            return image.loading_unit_id == loading_unit_id;
        });
    return found == images_.end() ? nullptr : &*found;
}

const RuntimeImage* RuntimeImageSet::FindByRuntimeRange(uintptr_t address, size_t size) const {
    const RuntimeImage* selected = nullptr;
    for (const auto& image : images_) {
        if (!image.ContainsRuntimeRange(address, size)) continue;
        if (selected != nullptr) return nullptr;
        selected = &image;
    }
    return selected;
}

const RuntimeImage* RuntimeImageSet::FindByModule(const ModuleImage& module) const {
    const RuntimeImage* selected = nullptr;
    for (const auto& image : images_) {
        if (!SameModule(image.module, module)) continue;
        if (selected != nullptr) return nullptr;
        selected = &image;
    }
    return selected;
}

bool RuntimeImageSet::SameIdentity(const RuntimeImageSet& other) const {
    if (images_.size() != other.images_.size()) return false;
    for (const auto& image : images_) {
        const auto found = std::find_if(other.images_.begin(), other.images_.end(),
                                        [&image](const RuntimeImage& candidate) {
                                            return SameImageIdentity(image, candidate);
                                        });
        if (found == other.images_.end()) return false;
    }
    return true;
}

void RuntimeImageSet::BindGeneration(uint64_t runtime_generation) {
    for (auto& image : images_) image.runtime_generation = runtime_generation;
}

void RuntimeImageSet::ResetLiveEntryBindings() {
    for (auto& image : images_) image.live_entry_count = 0;
}

bool RuntimeImageSet::RecordLiveEntry(RuntimeImageId id) {
    for (auto& image : images_) {
        if (image.id != id) continue;
        if (image.live_entry_count != UINT32_MAX) ++image.live_entry_count;
        return true;
    }
    return false;
}

bool RuntimeImageSet::BindLiveEntries(std::span<const RuntimeImageId> image_ids) {
    std::vector<uint32_t> counts(images_.size(), 0);
    for (const RuntimeImageId id : image_ids) {
        if (id == kInvalidRuntimeImageId) return false;
        const auto found = std::find_if(images_.begin(), images_.end(),
                                        [id](const RuntimeImage& image) { return image.id == id; });
        if (found == images_.end()) return false;
        const size_t index = static_cast<size_t>(found - images_.begin());
        if (counts[index] != UINT32_MAX) ++counts[index];
    }
    for (size_t index = 0; index < images_.size(); ++index) {
        images_[index].live_entry_count = counts[index];
    }
    return true;
}

bool RuntimeImageSet::BindDeferredProgramHash(uint32_t root_program_hash) {
    for (const auto& image : images_) {
        if (image.kind != RuntimeImageKind::kDeferred) continue;
        if (!image.snapshot.deferred_program_hash.has_value() ||
            *image.snapshot.deferred_program_hash != root_program_hash) {
            return false;
        }
    }
    for (auto& image : images_) {
        if (image.kind == RuntimeImageKind::kDeferred) {
            image.deferred_program_hash_vm_bound = true;
        }
    }
    return true;
}

std::optional<uint32_t> ParseDeferredLoadingUnitId(std::string_view root_module_name,
                                                   std::string_view candidate_module_name) {
    if (root_module_name.empty() || candidate_module_name.empty() ||
        root_module_name == candidate_module_name) {
        return std::nullopt;
    }
    std::string direct_prefix(root_module_name);
    direct_prefix.push_back('-');
    if (auto id = ParseDeferredSuffix(direct_prefix, candidate_module_name); id.has_value()) {
        return id;
    }
    if (root_module_name.ends_with(".so")) {
        std::string stem_prefix(root_module_name.substr(0, root_module_name.size() - 3));
        stem_prefix.push_back('-');
        if (auto id = ParseDeferredSuffix(stem_prefix, candidate_module_name); id.has_value()) {
            return id;
        }
    }
    return std::nullopt;
}

}  // namespace dartplant
