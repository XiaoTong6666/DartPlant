#ifndef DARTPLANT_RUNTIME_FLUTTER_SNAPSHOT_INTERNAL_H_
#define DARTPLANT_RUNTIME_FLUTTER_SNAPSHOT_INTERNAL_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/internal.h"
#include "dartplant/advanced/flutter_snapshot.h"
#include "elf/dynamic_view.h"
#include "elf/elf_types.h"

namespace dartplant {

struct DartSnapshotHeader {
    uint64_t declared_length = 0;
    uint64_t kind = 0;
    // First byte after Snapshot header + version hash + NUL-terminated feature
    // string. Deferred Full-AOT units place their uint32_t program hash here.
    uint64_t payload_offset = 0;
    std::string snapshot_hash;
    std::string features;
};

enum class ElfSectionLookupStatus : uint8_t {
    kFound = 0,
    kNotFound,
    kMalformed,
};

struct ElfSectionSymbol {
    uint64_t value = 0;
    uint64_t size = 0;
    uint64_t file_offset = 0;
    uint8_t info = 0;
    uint8_t other = 0;
    uint16_t section_index = SHN_UNDEF;
};

struct ElfSectionLookupResult {
    ElfSectionLookupStatus status = ElfSectionLookupStatus::kNotFound;
    ElfSectionSymbol symbol{};
};

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
    // Present only for a secondary AOT loading unit. Dart writes the same
    // ProgramVisitor::Hash value into every deferred snapshot and compares it
    // with the root program at load time. It proves same-program membership,
    // not the numeric LoadingUnit id (and is not a cryptographic identity).
    std::optional<uint32_t> deferred_program_hash;

    bool Matches(const ModuleImage& module) const;
    std::optional<uintptr_t> ResolveInstructionVa(const ModuleImage& module,
                                                  uint64_t instruction_va) const;
    std::optional<uintptr_t> ResolveInstructionRange(const ModuleImage& module,
                                                     uint64_t instruction_offset,
                                                     uint64_t instruction_size) const;
    std::optional<uintptr_t> ResolveInstructionOffset(const ModuleImage& module,
                                                      uint64_t instruction_offset) const;
};

std::optional<std::string> SelectFlutterSnapshotProfile(std::string_view features);

bool SnapshotSymbolMatchesContract(const ElfDynamicSymbol& symbol,
                                   std::span<const ElfProgramHeaderView> headers,
                                   uint32_t required_flags, bool loaded_image);
bool SnapshotSymbolMatchesContract(const ElfSectionSymbol& symbol,
                                   std::span<const ElfProgramHeaderView> headers,
                                   uint32_t required_flags, bool loaded_image);

std::optional<DartSnapshotHeader> ParseDartSnapshotHeader(std::span<const uint8_t> bytes);
std::optional<uint32_t> ParseDartDeferredProgramHash(std::span<const uint8_t> bytes,
                                                     const DartSnapshotHeader& header);

// Supplementary section-table symbol lookup used only when PT_DYNAMIC is
// structurally unavailable. Sections establish symbol consistency only; ELF
// VA to file-offset conversion is exclusively proven through PT_LOAD headers.
ElfSectionLookupResult FindElfSectionSymbol(std::span<const uint8_t> bytes,
                                            std::span<const ElfProgramHeaderView> headers,
                                            std::string_view wanted);

std::optional<FlutterSnapshotSource> DiscoverFlutterSnapshot(const ModuleImage& module,
                                                             std::string* error);
// Deferred AOT loading units emitted by the supported Flutter/Dart ELF
// producer retain the isolate snapshot symbol contract used by the root
// libapp.so. The separate entry point exists so callers can bind the result to
// a source-proven loading-unit id/program incarnation without conflating root
// and secondary image semantics.
std::optional<FlutterSnapshotSource> DiscoverDeferredFlutterSnapshot(const ModuleImage& module,
                                                                     std::string* error);

void FillSnapshotInfo(const FlutterSnapshotSource& snapshot, DartPlantFlutterSnapshotInfo* info);

}  // namespace dartplant

#endif  // DARTPLANT_RUNTIME_FLUTTER_SNAPSHOT_INTERNAL_H_
