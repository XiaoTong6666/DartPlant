#include <elf.h>
#include <link.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#include "elf/dynamic_view.h"
#include "runtime/flutter_snapshot_internal.h"
#include "runtime/runtime_internal.h"

namespace dartplant {
namespace {

struct ElfSymbol {
    uint64_t value = 0;
    uint64_t size = 0;
    uint64_t file_offset = 0;
};

struct LoadedSnapshotSymbols {
    uintptr_t isolate_data = 0;
    uint64_t isolate_data_size = 0;
    uintptr_t isolate_instructions = 0;
    uint64_t isolate_instructions_size = 0;
    uint64_t isolate_instructions_va = 0;
};

struct LoadedSnapshotLookup {
    const ModuleImage* module = nullptr;
    LoadedSnapshotSymbols symbols{};
    ElfDynamicLookupStatus status = ElfDynamicLookupStatus::kUnavailable;
    bool matched_module = false;
};

bool SnapshotDynamicSymbolIsValid(const ElfDynamicSymbol& symbol,
                                  std::span<const ElfProgramHeaderView> headers,
                                  uint32_t required_flags, bool loaded_image) {
    return symbol.value != 0 && symbol.size != 0 && ELF64_ST_TYPE(symbol.info) == STT_OBJECT &&
           ElfVaRangeHasFlags(headers, symbol.value, symbol.size, required_flags, loaded_image);
}

int FindLoadedSnapshotSymbols(dl_phdr_info* info, size_t, void* opaque) {
    auto* lookup = static_cast<LoadedSnapshotLookup*>(opaque);
    if (info == nullptr || lookup == nullptr || lookup->module == nullptr ||
        static_cast<uintptr_t>(info->dlpi_addr) != lookup->module->load_bias) {
        return 0;
    }
    lookup->matched_module = true;
    std::vector<ElfProgramHeaderView> headers;
    headers.reserve(info->dlpi_phnum);
    for (Elf64_Half index = 0; index < info->dlpi_phnum; ++index) {
        const Elf64_Phdr& phdr = info->dlpi_phdr[index];
        headers.push_back({
            .type = phdr.p_type,
            .flags = phdr.p_flags,
            .offset = phdr.p_offset,
            .virtual_address = phdr.p_vaddr,
            .file_size = phdr.p_filesz,
            .memory_size = phdr.p_memsz,
        });
    }
    const auto dynamic = FindElfProgramHeader(headers, PT_DYNAMIC);
    if (!dynamic.has_value()) {
        lookup->status = ElfDynamicLookupStatus::kUnavailable;
        return 1;
    }
    LoadedElfReader reader(info->dlpi_addr, headers);
    const auto view = ReadElfDynamicView(reader, dynamic->virtual_address, dynamic->memory_size);
    if (view.status == ElfDynamicViewStatus::kUnavailable) {
        lookup->status = ElfDynamicLookupStatus::kUnavailable;
        return 1;
    }
    if (view.status == ElfDynamicViewStatus::kMalformed) {
        lookup->status = ElfDynamicLookupStatus::kMalformed;
        return 1;
    }
    const auto data = FindElfDynamicSymbol(reader, view.view, "_kDartIsolateSnapshotData");
    if (data.status != ElfDynamicLookupStatus::kFound) {
        lookup->status = data.status;
        return 1;
    }
    const auto instructions =
        FindElfDynamicSymbol(reader, view.view, "_kDartIsolateSnapshotInstructions");
    if (instructions.status != ElfDynamicLookupStatus::kFound) {
        lookup->status = instructions.status;
        return 1;
    }
    if (!SnapshotDynamicSymbolIsValid(data.symbol, headers, PF_R, true) ||
        !SnapshotDynamicSymbolIsValid(instructions.symbol, headers, PF_R | PF_X, true) ||
        data.symbol.value > UINTPTR_MAX - info->dlpi_addr ||
        instructions.symbol.value > UINTPTR_MAX - info->dlpi_addr) {
        lookup->status = ElfDynamicLookupStatus::kMalformed;
        return 1;
    }
    lookup->symbols.isolate_data = info->dlpi_addr + data.symbol.value;
    lookup->symbols.isolate_data_size = data.symbol.size;
    lookup->symbols.isolate_instructions = info->dlpi_addr + instructions.symbol.value;
    lookup->symbols.isolate_instructions_size = instructions.symbol.size;
    lookup->symbols.isolate_instructions_va = instructions.symbol.value;
    lookup->status = ElfDynamicLookupStatus::kFound;
    return 1;
}

std::optional<std::vector<uint8_t>> ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
}

struct FileSnapshotLookup {
    ElfDynamicLookupStatus status = ElfDynamicLookupStatus::kUnavailable;
    ElfSymbol isolate_data{};
    ElfSymbol isolate_instructions{};
};

FileSnapshotLookup FindFileDynamicSnapshotSymbols(const std::vector<uint8_t>& bytes) {
    std::vector<ElfProgramHeaderView> headers;
    if (!ParseElf64ProgramHeaders(bytes, &headers)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const auto dynamic = FindElfProgramHeader(headers, PT_DYNAMIC);
    if (!dynamic.has_value()) return {.status = ElfDynamicLookupStatus::kUnavailable};
    FileElfReader reader(bytes, headers);
    const auto view = ReadElfDynamicView(reader, dynamic->virtual_address, dynamic->file_size);
    if (view.status == ElfDynamicViewStatus::kUnavailable) {
        return {.status = ElfDynamicLookupStatus::kUnavailable};
    }
    if (view.status == ElfDynamicViewStatus::kMalformed) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const auto data = FindElfDynamicSymbol(reader, view.view, "_kDartIsolateSnapshotData");
    if (data.status != ElfDynamicLookupStatus::kFound) return {.status = data.status};
    const auto instructions =
        FindElfDynamicSymbol(reader, view.view, "_kDartIsolateSnapshotInstructions");
    if (instructions.status != ElfDynamicLookupStatus::kFound) {
        return {.status = instructions.status};
    }
    if (!SnapshotDynamicSymbolIsValid(data.symbol, headers, PF_R, false) ||
        !SnapshotDynamicSymbolIsValid(instructions.symbol, headers, PF_R | PF_X, false)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const auto data_offset =
        ElfVaToFileOffset(headers, data.symbol.value, static_cast<size_t>(data.symbol.size));
    const auto instruction_offset = ElfVaToFileOffset(
        headers, instructions.symbol.value, static_cast<size_t>(instructions.symbol.size));
    if (!data_offset.has_value() || !instruction_offset.has_value()) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    return {
        .status = ElfDynamicLookupStatus::kFound,
        .isolate_data = {data.symbol.value, data.symbol.size, *data_offset},
        .isolate_instructions = {instructions.symbol.value, instructions.symbol.size,
                                 *instruction_offset},
    };
}

std::optional<ElfSymbol> FindSymbol(const std::vector<uint8_t>& bytes, std::string_view wanted) {
    if (bytes.size() < sizeof(Elf64_Ehdr)) return std::nullopt;
    const auto* header = reinterpret_cast<const Elf64_Ehdr*>(bytes.data());
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 || header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_shentsize != sizeof(Elf64_Shdr)) {
        return std::nullopt;
    }
    const size_t section_bytes = static_cast<size_t>(header->e_shnum) * header->e_shentsize;
    if (header->e_shoff > bytes.size() || section_bytes > bytes.size() - header->e_shoff) {
        return std::nullopt;
    }
    const auto* sections = reinterpret_cast<const Elf64_Shdr*>(bytes.data() + header->e_shoff);
    for (size_t section_index = 0; section_index < header->e_shnum; ++section_index) {
        const Elf64_Shdr& section = sections[section_index];
        if (section.sh_type != SHT_DYNSYM && section.sh_type != SHT_SYMTAB) continue;
        if (section.sh_link >= header->e_shnum) continue;
        const Elf64_Shdr& strings = sections[section.sh_link];
        if (section.sh_entsize != sizeof(Elf64_Sym) || section.sh_offset > bytes.size() ||
            section.sh_size > bytes.size() - section.sh_offset ||
            strings.sh_offset > bytes.size() ||
            strings.sh_size > bytes.size() - strings.sh_offset) {
            continue;
        }
        const auto* symbols = reinterpret_cast<const Elf64_Sym*>(bytes.data() + section.sh_offset);
        const auto* names = reinterpret_cast<const char*>(bytes.data() + strings.sh_offset);
        const size_t count = section.sh_size / section.sh_entsize;
        for (size_t index = 0; index < count; ++index) {
            const Elf64_Sym& symbol = symbols[index];
            if (symbol.st_name >= strings.sh_size || symbol.st_value == 0 ||
                wanted != names + symbol.st_name) {
                continue;
            }
            for (size_t data_section = 0; data_section < header->e_shnum; ++data_section) {
                const Elf64_Shdr& owner = sections[data_section];
                if (owner.sh_addr <= symbol.st_value &&
                    symbol.st_value - owner.sh_addr <= owner.sh_size &&
                    symbol.st_value - owner.sh_addr <= UINT64_MAX - owner.sh_offset) {
                    return ElfSymbol{symbol.st_value, symbol.st_size,
                                     owner.sh_offset + (symbol.st_value - owner.sh_addr)};
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<DartSnapshotHeader> ReadSnapshotHeader(const std::vector<uint8_t>& bytes,
                                                     const ElfSymbol& symbol) {
    if (symbol.file_offset > bytes.size() || symbol.size > bytes.size() - symbol.file_offset) {
        return std::nullopt;
    }
    return ParseDartSnapshotHeader(std::span<const uint8_t>(
        bytes.data() + static_cast<size_t>(symbol.file_offset), static_cast<size_t>(symbol.size)));
}

std::optional<DartSnapshotHeader> ReadSnapshotHeader(uintptr_t address, uint64_t size) {
    if (address == 0 || size > SIZE_MAX) return std::nullopt;
    return ParseDartSnapshotHeader(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(address), static_cast<size_t>(size)));
}

bool HasFeature(std::string_view features, std::string_view feature) {
    while (!features.empty()) {
        const size_t separator = features.find(' ');
        const std::string_view token = features.substr(0, separator);
        if (token == feature) return true;
        if (separator == std::string_view::npos) break;
        features.remove_prefix(separator + 1);
    }
    return false;
}

}  // namespace

std::optional<DartSnapshotHeader> ParseDartSnapshotHeader(std::span<const uint8_t> bytes) {
    constexpr int32_t kMagicValue = static_cast<int32_t>(0xdcdcf5f5U);
    constexpr size_t kMagicOffset = 0;
    constexpr size_t kMagicSize = sizeof(int32_t);
    constexpr size_t kLengthOffset = kMagicOffset + kMagicSize;
    constexpr size_t kLengthSize = sizeof(int64_t);
    constexpr size_t kKindOffset = kLengthOffset + kLengthSize;
    constexpr size_t kKindSize = sizeof(int64_t);
    constexpr size_t kHeaderSize = kKindOffset + kKindSize;
    constexpr size_t kHashSize = 32;
    constexpr int64_t kFullAotKind = 3;
    constexpr size_t kMinimumSize = kHeaderSize + kHashSize + 1;
    if (bytes.size() < kMinimumSize) return std::nullopt;

    int32_t magic = 0;
    int64_t stored_length = 0;
    int64_t kind = 0;
    memcpy(&magic, bytes.data() + kMagicOffset, sizeof(magic));
    memcpy(&stored_length, bytes.data() + kLengthOffset, sizeof(stored_length));
    memcpy(&kind, bytes.data() + kKindOffset, sizeof(kind));
    if (magic != kMagicValue || stored_length < 0 || kind != kFullAotKind ||
        static_cast<uint64_t>(stored_length) > UINT64_MAX - kMagicSize) {
        return std::nullopt;
    }
    const uint64_t declared_length = static_cast<uint64_t>(stored_length) + kMagicSize;
    if (declared_length < kMinimumSize || declared_length > bytes.size()) return std::nullopt;

    const auto* hash = bytes.data() + kHeaderSize;
    if (!std::all_of(hash, hash + kHashSize, [](uint8_t value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                   (value >= 'A' && value <= 'F');
        })) {
        return std::nullopt;
    }
    const char* features = reinterpret_cast<const char*>(hash + kHashSize);
    const size_t remaining = static_cast<size_t>(declared_length) - kHeaderSize - kHashSize;
    const size_t feature_length = strnlen(features, remaining);
    if (feature_length == remaining) return std::nullopt;
    return DartSnapshotHeader{
        .declared_length = declared_length,
        .kind = static_cast<uint64_t>(kind),
        .snapshot_hash = std::string(reinterpret_cast<const char*>(hash), kHashSize),
        .features = std::string(features, feature_length),
    };
}

std::optional<std::string> SelectFlutterSnapshotProfile(std::string_view features) {
    const bool arm64 = HasFeature(features, "arm64");
    const bool android = HasFeature(features, "android");
    const bool product = HasFeature(features, "product");
    const bool nonproduct = HasFeature(features, "release");
    const bool compressed = HasFeature(features, "compressed-pointers") &&
                            !HasFeature(features, "no-compressed-pointers");
    if (!arm64 || !android || product == nonproduct) return std::nullopt;
    if (product && compressed) return "flutter-arm64-product-compressed";
    if (product && !compressed) return "flutter-arm64-product-uncompressed";
    if (nonproduct && compressed) return "flutter-arm64-profile-compressed";
    return "flutter-arm64-profile-uncompressed";
}

void FillSnapshotInfo(const FlutterSnapshotSource& snapshot, DartPlantFlutterSnapshotInfo* info) {
    if (info == nullptr) return;
    info->module_name = snapshot.module_name.c_str();
    info->module_path = snapshot.module_path.c_str();
    info->module_build_id = snapshot.module_build_id.c_str();
    info->snapshot_hash = snapshot.snapshot_hash.c_str();
    info->snapshot_features = snapshot.snapshot_features.c_str();
    info->profile_name = snapshot.profile_name.c_str();
    info->load_bias = snapshot.isolate_instructions_runtime - snapshot.isolate_instructions_va;
    info->isolate_instructions_va = snapshot.isolate_instructions_va;
    info->isolate_instructions_size = snapshot.isolate_instructions_size;
    info->isolate_instructions_runtime = snapshot.isolate_instructions_runtime;
    info->compressed_pointers = snapshot.compressed_pointers ? 1 : 0;
}

bool FlutterSnapshotSource::Matches(const ModuleImage& module) const {
    return module.name == module_name && module.path == module_path &&
           EqualsIgnoreCaseAscii(module.build_id, module_build_id);
}

std::optional<uintptr_t> FlutterSnapshotSource::ResolveInstructionVa(
    const ModuleImage& module, uint64_t instruction_va) const {
    if (!Matches(module) || instruction_va < isolate_instructions_va ||
        instruction_va - isolate_instructions_va >= isolate_instructions_size) {
        return std::nullopt;
    }
    const uintptr_t target = isolate_instructions_runtime +
                             static_cast<uintptr_t>(instruction_va - isolate_instructions_va);
    return module.ContainsExecutable(target, 4) ? std::optional<uintptr_t>(target) : std::nullopt;
}

std::optional<uintptr_t> FlutterSnapshotSource::ResolveInstructionOffset(
    const ModuleImage& module, uint64_t instruction_offset) const {
    if (!Matches(module) || instruction_offset >= isolate_instructions_size ||
        instruction_offset > UINTPTR_MAX - isolate_instructions_runtime) {
        return std::nullopt;
    }
    const uintptr_t target = isolate_instructions_runtime + instruction_offset;
    return module.ContainsExecutable(target, 4) ? std::optional<uintptr_t>(target) : std::nullopt;
}

std::optional<FlutterSnapshotSource> DiscoverFlutterSnapshot(const ModuleImage& module,
                                                             std::string* error) {
    const auto build_source =
        [&](const DartSnapshotHeader& header, uint64_t instructions_va, uint64_t instructions_size,
            uintptr_t instructions_runtime) -> std::optional<FlutterSnapshotSource> {
        FlutterSnapshotSource source;
        source.module_name = module.name;
        source.module_path = module.path;
        source.module_build_id = module.build_id;
        source.snapshot_hash = header.snapshot_hash;
        source.snapshot_features = header.features;
        const auto profile = SelectFlutterSnapshotProfile(source.snapshot_features);
        if (!profile.has_value()) {
            if (error != nullptr) *error = "Flutter snapshot feature profile is unsupported";
            return std::nullopt;
        }
        source.profile_name = *profile;
        source.isolate_instructions_va = instructions_va;
        source.isolate_instructions_size = instructions_size;
        source.isolate_instructions_runtime = instructions_runtime;
        source.compressed_pointers =
            HasFeature(source.snapshot_features, "compressed-pointers") &&
            !HasFeature(source.snapshot_features, "no-compressed-pointers");
        return source;
    };

    // Dynamic symbols are the canonical Dart AOT artifact contract. Resolve
    // them from the loaded PT_DYNAMIC view first so discovery does not depend
    // on section headers or the backing file remaining readable after load.
    LoadedSnapshotLookup loaded{.module = &module};
    dl_iterate_phdr(FindLoadedSnapshotSymbols, &loaded);
    if (loaded.matched_module && loaded.status == ElfDynamicLookupStatus::kFound) {
        const auto header =
            ReadSnapshotHeader(loaded.symbols.isolate_data, loaded.symbols.isolate_data_size);
        if (!header.has_value()) {
            if (error != nullptr) *error = "loaded Dart isolate snapshot header is not recognized";
            return std::nullopt;
        }
        return build_source(*header, loaded.symbols.isolate_instructions_va,
                            loaded.symbols.isolate_instructions_size,
                            loaded.symbols.isolate_instructions);
    }
    if (loaded.matched_module && loaded.status != ElfDynamicLookupStatus::kUnavailable) {
        if (error != nullptr) {
            *error = loaded.status == ElfDynamicLookupStatus::kMalformed
                         ? "loaded Flutter app PT_DYNAMIC snapshot symbols are malformed"
                         : "loaded Flutter app PT_DYNAMIC has no Dart isolate snapshot symbols";
        }
        return std::nullopt;
    }

    const auto bytes = ReadFile(module.path);
    if (!bytes.has_value()) {
        if (error != nullptr) *error = "cannot read Flutter app module or its dynamic symbols";
        return std::nullopt;
    }

    // The same PT_DYNAMIC semantics work for the file image, except ELF VAs
    // are translated through PT_LOAD p_filesz instead of load_bias+p_vaddr.
    const FileSnapshotLookup dynamic = FindFileDynamicSnapshotSymbols(*bytes);
    if (dynamic.status == ElfDynamicLookupStatus::kFound) {
        const auto header = ReadSnapshotHeader(*bytes, dynamic.isolate_data);
        if (!header.has_value()) {
            if (error != nullptr) *error = "Dart isolate snapshot header is not recognized";
            return std::nullopt;
        }
        if (dynamic.isolate_instructions.value > UINTPTR_MAX - module.load_bias) {
            if (error != nullptr)
                *error = "Dart isolate snapshot instructions VA overflows runtime";
            return std::nullopt;
        }
        const uintptr_t runtime = module.load_bias + dynamic.isolate_instructions.value;
        if (!module.ContainsExecutable(runtime, 4)) {
            if (error != nullptr) *error = "Dart isolate snapshot instructions are not executable";
            return std::nullopt;
        }
        return build_source(*header, dynamic.isolate_instructions.value,
                            dynamic.isolate_instructions.size, runtime);
    }
    if (dynamic.status != ElfDynamicLookupStatus::kUnavailable) {
        if (error != nullptr) {
            *error = dynamic.status == ElfDynamicLookupStatus::kMalformed
                         ? "Flutter app PT_DYNAMIC snapshot symbols are malformed"
                         : "Flutter app PT_DYNAMIC has no Dart isolate snapshot symbols";
        }
        return std::nullopt;
    }

    // A section-table lookup is supplementary compatibility for images whose
    // dynamic symbol view is structurally unavailable. It never masks a
    // malformed or authoritative PT_DYNAMIC not-found result.
    const auto isolate_data = FindSymbol(*bytes, "_kDartIsolateSnapshotData");
    const auto isolate_instr = FindSymbol(*bytes, "_kDartIsolateSnapshotInstructions");
    if (!isolate_data.has_value() || !isolate_instr.has_value() || isolate_instr->size == 0) {
        if (error != nullptr) *error = "loaded app module has no Dart isolate snapshot symbols";
        return std::nullopt;
    }
    const auto header = ReadSnapshotHeader(*bytes, *isolate_data);
    if (!header.has_value()) {
        if (error != nullptr) *error = "Dart isolate snapshot header is not recognized";
        return std::nullopt;
    }
    if (isolate_instr->value > UINTPTR_MAX - module.load_bias) {
        if (error != nullptr) *error = "Dart isolate snapshot instructions VA overflows runtime";
        return std::nullopt;
    }
    return build_source(*header, isolate_instr->value, isolate_instr->size,
                        module.load_bias + isolate_instr->value);
}

}  // namespace dartplant
