#ifndef DARTPLANT_RUNTIME_FLUTTER_SNAPSHOT_INTERNAL_H_
#define DARTPLANT_RUNTIME_FLUTTER_SNAPSHOT_INTERNAL_H_

#include <optional>
#include <string>
#include <string_view>

#include "core/internal.h"
#include "dartplant/flutter_snapshot.h"

namespace dartplant {

struct FlutterSnapshotSource {
    std::string module_name;
    std::string module_path;
    std::string module_build_id;
    std::string snapshot_hash;
    std::string snapshot_features;
    std::string profile_name;
    uint64_t isolate_instructions_va = 0;
    uint64_t isolate_instructions_size = 0;
    uintptr_t isolate_instructions_runtime = 0;
    bool compressed_pointers = false;

    bool Matches(const ModuleImage& module) const;
    std::optional<uintptr_t> ResolveInstructionVa(const ModuleImage& module,
                                                  uint64_t instruction_va) const;
};

std::optional<std::string> SelectFlutterSnapshotProfile(std::string_view features);

std::optional<FlutterSnapshotSource> DiscoverFlutterSnapshot(const ModuleImage& module,
                                                             std::string* error);

void FillSnapshotInfo(const FlutterSnapshotSource& snapshot, DartPlantFlutterSnapshotInfo* info);

}  // namespace dartplant

#endif  // DARTPLANT_RUNTIME_FLUTTER_SNAPSHOT_INTERNAL_H_
