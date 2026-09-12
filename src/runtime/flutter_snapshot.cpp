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

struct LoadedSnapshotSymbols {
    uintptr_t isolate_data = 0;
    uint64_t isolate_data_size = 0;
    uintptr_t isolate_instructions = 0;
    uint64_t isolate_instructions_size = 0;
    uint64_t isolate_instructions_va = 0;
};

struct LoadedSnapshotLookup {
    const ModuleImage* module = nullptr;
    const char* data_symbol = nullptr;
    const char* instructions_symbol = nullptr;
    LoadedSnapshotSymbols symbols{};
    ElfDynamicLookupStatus status = ElfDynamicLookupStatus::kUnavailable;
    bool matched_module = false;
};

int FindLoadedSnapshotSymbols(dl_phdr_info* info, size_t, void* opaque) {
    auto* lookup = static_cast<LoadedSnapshotLookup*>(opaque);
    if (info == nullptr || lookup == nullptr || lookup->module == nullptr ||
        lookup->data_symbol == nullptr || lookup->instructions_symbol == nullptr ||
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
            .alignment = phdr.p_align,
        });
    }
    if (!ValidateElfLoadLayout(headers)) {
        lookup->status = ElfDynamicLookupStatus::kMalformed;
        return 1;
    }
    const auto dynamic = FindElfProgramHeader(headers, PT_DYNAMIC);
    if (dynamic.status == ElfProgramHeaderLookupStatus::kMissing) {
        lookup->status = ElfDynamicLookupStatus::kUnavailable;
        return 1;
    }
    if (dynamic.status == ElfProgramHeaderLookupStatus::kAmbiguous) {
        lookup->status = ElfDynamicLookupStatus::kMalformed;
        return 1;
    }
    if (!ElfProgramHeaderHasCanonicalFileBacking(headers, dynamic.header, PF_R)) {
        lookup->status = ElfDynamicLookupStatus::kMalformed;
        return 1;
    }
    FileBackedLoadedElfReader reader(info->dlpi_addr, headers);
    const auto view =
        ReadElfDynamicView(reader, dynamic.header.virtual_address, dynamic.header.file_size);
    if (view.status == ElfDynamicViewStatus::kMalformed) {
        lookup->status = ElfDynamicLookupStatus::kMalformed;
        return 1;
    }
    const auto data = FindElfDynamicSymbol(reader, view.view, lookup->data_symbol);
    if (data.status != ElfDynamicLookupStatus::kFound) {
        lookup->status = data.status;
        return 1;
    }
    const auto instructions = FindElfDynamicSymbol(reader, view.view, lookup->instructions_symbol);
    if (instructions.status != ElfDynamicLookupStatus::kFound) {
        lookup->status = instructions.status;
        return 1;
    }
    if (!SnapshotSymbolMatchesContract(data.symbol, headers, PF_R, false) ||
        !SnapshotSymbolMatchesContract(instructions.symbol, headers, PF_R | PF_X, false) ||
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
    ElfSectionSymbol isolate_data{};
    ElfSectionSymbol isolate_instructions{};
};

FileSnapshotLookup FindFileDynamicSnapshotSymbols(const std::vector<uint8_t>& bytes,
                                                  std::string_view data_symbol,
                                                  std::string_view instructions_symbol) {
    std::vector<ElfProgramHeaderView> headers;
    if (!ParseElf64ProgramHeaders(bytes, &headers)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const auto dynamic = FindElfProgramHeader(headers, PT_DYNAMIC);
    if (dynamic.status == ElfProgramHeaderLookupStatus::kMissing) {
        return {.status = ElfDynamicLookupStatus::kUnavailable};
    }
    if (dynamic.status == ElfProgramHeaderLookupStatus::kAmbiguous) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    if (!ElfProgramHeaderHasCanonicalFileBacking(headers, dynamic.header, PF_R)) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    FileElfReader reader(bytes, headers);
    const auto view =
        ReadElfDynamicView(reader, dynamic.header.virtual_address, dynamic.header.file_size);
    if (view.status == ElfDynamicViewStatus::kMalformed) {
        return {.status = ElfDynamicLookupStatus::kMalformed};
    }
    const auto data = FindElfDynamicSymbol(reader, view.view, data_symbol);
    if (data.status != ElfDynamicLookupStatus::kFound) return {.status = data.status};
    const auto instructions = FindElfDynamicSymbol(reader, view.view, instructions_symbol);
    if (instructions.status != ElfDynamicLookupStatus::kFound) {
        return {.status = instructions.status};
    }
    if (!SnapshotSymbolMatchesContract(data.symbol, headers, PF_R, false) ||
        !SnapshotSymbolMatchesContract(instructions.symbol, headers, PF_R | PF_X, false)) {
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
        .isolate_data =
            {
                .value = data.symbol.value,
                .size = data.symbol.size,
                .file_offset = *data_offset,
                .info = data.symbol.info,
                .other = data.symbol.other,
                .section_index = data.symbol.section_index,
            },
        .isolate_instructions =
            {
                .value = instructions.symbol.value,
                .size = instructions.symbol.size,
                .file_offset = *instruction_offset,
                .info = instructions.symbol.info,
                .other = instructions.symbol.other,
                .section_index = instructions.symbol.section_index,
            },
    };
}

std::optional<DartSnapshotHeader> ReadSnapshotHeader(const std::vector<uint8_t>& bytes,
                                                     const ElfSectionSymbol& symbol) {
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

std::optional<uint32_t> ReadDeferredProgramHash(const std::vector<uint8_t>& bytes,
                                                const ElfSectionSymbol& symbol,
                                                const DartSnapshotHeader& header) {
    if (symbol.file_offset > bytes.size() || symbol.size > bytes.size() - symbol.file_offset ||
        symbol.size > SIZE_MAX) {
        return std::nullopt;
    }
    return ParseDartDeferredProgramHash(
        std::span<const uint8_t>(bytes.data() + static_cast<size_t>(symbol.file_offset),
                                 static_cast<size_t>(symbol.size)),
        header);
}

std::optional<uint32_t> ReadDeferredProgramHash(uintptr_t address, uint64_t size,
                                                const DartSnapshotHeader& header) {
    if (address == 0 || size > SIZE_MAX) return std::nullopt;
    return ParseDartDeferredProgramHash(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(address),
                                 static_cast<size_t>(size)),
        header);
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

namespace {

bool SnapshotSymbolMatchesContract(uint64_t value, uint64_t size, uint8_t info,
                                   uint16_t section_index,
                                   std::span<const ElfProgramHeaderView> headers,
                                   uint32_t required_flags, bool loaded_image) {
    return value != 0 && size != 0 && section_index != SHN_UNDEF && section_index < SHN_LORESERVE &&
           ELF64_ST_BIND(info) == STB_GLOBAL && ELF64_ST_TYPE(info) == STT_OBJECT &&
           ElfVaRangeHasFlags(headers, value, size, required_flags, loaded_image);
}

}  // namespace

bool SnapshotSymbolMatchesContract(const ElfDynamicSymbol& symbol,
                                   std::span<const ElfProgramHeaderView> headers,
                                   uint32_t required_flags, bool loaded_image) {
    return SnapshotSymbolMatchesContract(symbol.value, symbol.size, symbol.info,
                                         symbol.section_index, headers, required_flags,
                                         loaded_image);
}

bool SnapshotSymbolMatchesContract(const ElfSectionSymbol& symbol,
                                   std::span<const ElfProgramHeaderView> headers,
                                   uint32_t required_flags, bool loaded_image) {
    return SnapshotSymbolMatchesContract(symbol.value, symbol.size, symbol.info,
                                         symbol.section_index, headers, required_flags,
                                         loaded_image);
}

ElfSectionLookupResult FindElfSectionSymbol(std::span<const uint8_t> bytes,
                                            std::span<const ElfProgramHeaderView> headers,
                                            std::string_view wanted) {
    if (wanted.empty() || bytes.size() < sizeof(Elf64_Ehdr)) {
        return {.status = ElfSectionLookupStatus::kMalformed};
    }

    Elf64_Ehdr header{};
    memcpy(&header, bytes.data(), sizeof(header));
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_shentsize != sizeof(Elf64_Shdr)) {
        return {.status = ElfSectionLookupStatus::kMalformed};
    }
    // ELF extended section numbering encodes the real section count in
    // section-header[0].sh_size when e_shnum == 0 and a section table is
    // present. DartPlant's section path is only a compatibility fallback for
    // current Dart AOT producers, so do not silently reinterpret an extended
    // table as "no sections". Keep the producer scope explicit and fail
    // closed until SHN_XINDEX/SHT_SYMTAB_SHNDX are implemented together.
    if (header.e_shnum == 0) {
        return {.status = header.e_shoff == 0 ? ElfSectionLookupStatus::kNotFound
                                              : ElfSectionLookupStatus::kMalformed};
    }
    if (header.e_shoff > bytes.size() ||
        static_cast<uint64_t>(header.e_shnum) >
            (bytes.size() - static_cast<size_t>(header.e_shoff)) / sizeof(Elf64_Shdr)) {
        return {.status = ElfSectionLookupStatus::kMalformed};
    }

    const auto read_section = [&](size_t index, Elf64_Shdr* out) {
        if (out == nullptr || index >= header.e_shnum) return false;
        const uint64_t offset = header.e_shoff + uint64_t{index} * sizeof(Elf64_Shdr);
        if (offset > bytes.size() || sizeof(Elf64_Shdr) > bytes.size() - offset) return false;
        memcpy(out, bytes.data() + static_cast<size_t>(offset), sizeof(*out));
        return true;
    };

    size_t dynsym_count = 0;
    size_t symtab_count = 0;
    for (size_t section_index = 0; section_index < header.e_shnum; ++section_index) {
        Elf64_Shdr section{};
        if (!read_section(section_index, &section)) {
            return {.status = ElfSectionLookupStatus::kMalformed};
        }
        dynsym_count += section.sh_type == SHT_DYNSYM ? 1 : 0;
        symtab_count += section.sh_type == SHT_SYMTAB ? 1 : 0;
    }
    if (dynsym_count > 1 || (dynsym_count == 0 && symtab_count > 1)) {
        return {.status = ElfSectionLookupStatus::kMalformed};
    }
    if (dynsym_count == 0 && symtab_count == 0) {
        return {.status = ElfSectionLookupStatus::kNotFound};
    }
    const uint32_t authoritative_type = dynsym_count == 1 ? SHT_DYNSYM : SHT_SYMTAB;
    struct ResolvedSymbol {
        uint64_t value;
        uint64_t size;
        uint8_t info;
        uint8_t other;
        uint16_t section_index;
        uint64_t file_offset;
    };
    std::optional<ResolvedSymbol> resolved_symbol;

    for (size_t section_index = 0; section_index < header.e_shnum; ++section_index) {
        Elf64_Shdr section{};
        if (!read_section(section_index, &section)) {
            return {.status = ElfSectionLookupStatus::kMalformed};
        }
        if (section.sh_type != authoritative_type) continue;
        if (section.sh_link >= header.e_shnum || section.sh_entsize != sizeof(Elf64_Sym) ||
            (section.sh_size % section.sh_entsize) != 0 || section.sh_offset > bytes.size() ||
            section.sh_size > bytes.size() - section.sh_offset) {
            return {.status = ElfSectionLookupStatus::kMalformed};
        }

        Elf64_Shdr strings{};
        if (!read_section(section.sh_link, &strings) || strings.sh_type != SHT_STRTAB ||
            strings.sh_offset > bytes.size() ||
            strings.sh_size > bytes.size() - strings.sh_offset) {
            return {.status = ElfSectionLookupStatus::kMalformed};
        }

        const size_t count = static_cast<size_t>(section.sh_size / section.sh_entsize);
        for (size_t index = 0; index < count; ++index) {
            Elf64_Sym symbol{};
            const uint64_t symbol_offset = section.sh_offset + uint64_t{index} * sizeof(Elf64_Sym);
            if (symbol_offset > bytes.size() || sizeof(symbol) > bytes.size() - symbol_offset) {
                return {.status = ElfSectionLookupStatus::kMalformed};
            }
            memcpy(&symbol, bytes.data() + static_cast<size_t>(symbol_offset), sizeof(symbol));
            if (symbol.st_name >= strings.sh_size) {
                return {.status = ElfSectionLookupStatus::kMalformed};
            }
            const size_t name_offset = static_cast<size_t>(strings.sh_offset + symbol.st_name);
            const size_t name_limit = static_cast<size_t>(strings.sh_size - symbol.st_name);
            const void* terminator = memchr(bytes.data() + name_offset, '\0', name_limit);
            if (terminator == nullptr) {
                return {.status = ElfSectionLookupStatus::kMalformed};
            }
            const auto* end = static_cast<const uint8_t*>(terminator);
            const size_t name_size = end - (bytes.data() + name_offset);
            if (name_size != wanted.size() ||
                memcmp(bytes.data() + name_offset, wanted.data(), wanted.size()) != 0) {
                continue;
            }

            if (symbol.st_value == 0) {
                return {.status = ElfSectionLookupStatus::kMalformed};
            }
            if (symbol.st_shndx == SHN_XINDEX || symbol.st_shndx == SHN_UNDEF ||
                symbol.st_shndx >= header.e_shnum || symbol.st_size == 0 ||
                symbol.st_size > SIZE_MAX || ELF64_ST_BIND(symbol.st_info) != STB_GLOBAL ||
                ELF64_ST_TYPE(symbol.st_info) != STT_OBJECT) {
                return {.status = ElfSectionLookupStatus::kMalformed};
            }
            Elf64_Shdr owner{};
            if (!read_section(symbol.st_shndx, &owner) || (owner.sh_flags & SHF_ALLOC) == 0 ||
                owner.sh_type == SHT_NOBITS || owner.sh_addr > symbol.st_value ||
                owner.sh_offset > bytes.size() || owner.sh_size > bytes.size() - owner.sh_offset) {
                return {.status = ElfSectionLookupStatus::kMalformed};
            }
            const uint64_t section_delta = symbol.st_value - owner.sh_addr;
            if (section_delta >= owner.sh_size || symbol.st_size > owner.sh_size - section_delta ||
                owner.sh_offset > UINT64_MAX - section_delta) {
                return {.status = ElfSectionLookupStatus::kMalformed};
            }
            const uint64_t section_file_offset = owner.sh_offset + section_delta;
            const auto file_offset =
                ElfVaToFileOffset(headers, symbol.st_value, static_cast<size_t>(symbol.st_size));
            if (!file_offset.has_value() || *file_offset != section_file_offset ||
                *file_offset > bytes.size() || symbol.st_size > bytes.size() - *file_offset) {
                return {.status = ElfSectionLookupStatus::kMalformed};
            }
            const ResolvedSymbol candidate = {
                .value = symbol.st_value,
                .size = symbol.st_size,
                .info = symbol.st_info,
                .other = symbol.st_other,
                .section_index = symbol.st_shndx,
                .file_offset = *file_offset,
            };
            if (resolved_symbol.has_value() &&
                (resolved_symbol->value != candidate.value ||
                 resolved_symbol->size != candidate.size ||
                 resolved_symbol->info != candidate.info ||
                 resolved_symbol->other != candidate.other ||
                 resolved_symbol->section_index != candidate.section_index ||
                 resolved_symbol->file_offset != candidate.file_offset)) {
                return {.status = ElfSectionLookupStatus::kMalformed};
            }
            resolved_symbol = candidate;
        }
    }
    if (resolved_symbol.has_value()) {
        return {
            .status = ElfSectionLookupStatus::kFound,
            .symbol =
                {
                    .value = resolved_symbol->value,
                    .size = resolved_symbol->size,
                    .file_offset = resolved_symbol->file_offset,
                    .info = resolved_symbol->info,
                    .other = resolved_symbol->other,
                    .section_index = resolved_symbol->section_index,
                },
        };
    }
    return {.status = ElfSectionLookupStatus::kNotFound};
}

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
    const uint64_t payload_offset =
        static_cast<uint64_t>(kHeaderSize + kHashSize + feature_length + 1);
    if (payload_offset > declared_length) return std::nullopt;
    return DartSnapshotHeader{
        .declared_length = declared_length,
        .kind = static_cast<uint64_t>(kind),
        .payload_offset = payload_offset,
        .snapshot_hash = std::string(reinterpret_cast<const char*>(hash), kHashSize),
        .features = std::string(features, feature_length),
    };
}

std::optional<uint32_t> ParseDartDeferredProgramHash(std::span<const uint8_t> bytes,
                                                     const DartSnapshotHeader& header) {
    constexpr uint64_t kProgramHashSize = sizeof(uint32_t);
    if (header.payload_offset > header.declared_length ||
        kProgramHashSize > header.declared_length - header.payload_offset ||
        header.declared_length > bytes.size()) {
        return std::nullopt;
    }
    const size_t offset = static_cast<size_t>(header.payload_offset);
    // Supported DartPlant AOT producers are Android ARM64 little-endian. Read
    // the serialized uint32_t explicitly so host endianness never leaks into
    // artifact provenance.
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
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
    return ResolveInstructionRange(module, instruction_va - isolate_instructions_va, 4);
}

std::optional<uintptr_t> FlutterSnapshotSource::ResolveInstructionRange(
    const ModuleImage& module, uint64_t instruction_offset, uint64_t instruction_size) const {
    if (!Matches(module) || instruction_size == 0 ||
        instruction_offset >= isolate_instructions_size ||
        instruction_size > isolate_instructions_size - instruction_offset ||
        instruction_size > SIZE_MAX ||
        instruction_offset > UINTPTR_MAX - isolate_instructions_runtime) {
        return std::nullopt;
    }
    const uintptr_t target =
        isolate_instructions_runtime + static_cast<uintptr_t>(instruction_offset);
    return module.ContainsExecutable(target, static_cast<size_t>(instruction_size))
               ? std::optional<uintptr_t>(target)
               : std::nullopt;
}

std::optional<uintptr_t> FlutterSnapshotSource::ResolveInstructionOffset(
    const ModuleImage& module, uint64_t instruction_offset) const {
    return ResolveInstructionRange(module, instruction_offset, 4);
}

namespace {

std::optional<FlutterSnapshotSource> DiscoverFlutterSnapshotWithSymbols(
    const ModuleImage& module, const char* data_symbol, const char* instructions_symbol,
    bool deferred_unit, std::string* error) {
    if (data_symbol == nullptr || instructions_symbol == nullptr) {
        if (error != nullptr) *error = "Flutter snapshot symbol contract is invalid";
        return std::nullopt;
    }
    const auto build_source =
        [&](const DartSnapshotHeader& header, std::optional<uint32_t> deferred_program_hash,
            uint64_t instructions_va, uint64_t instructions_size,
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
        source.deferred_program_hash = deferred_program_hash;
        return source;
    };

    // Dynamic symbols are the canonical Dart AOT artifact contract. Resolve
    // them from the loaded PT_DYNAMIC view first so discovery does not depend
    // on section headers or the backing file remaining readable after load.
    LoadedSnapshotLookup loaded{
        .module = &module,
        .data_symbol = data_symbol,
        .instructions_symbol = instructions_symbol,
    };
    dl_iterate_phdr(FindLoadedSnapshotSymbols, &loaded);
    if (loaded.matched_module && loaded.status == ElfDynamicLookupStatus::kFound) {
        const auto header =
            ReadSnapshotHeader(loaded.symbols.isolate_data, loaded.symbols.isolate_data_size);
        if (!header.has_value()) {
            if (error != nullptr) *error = "loaded Dart isolate snapshot header is not recognized";
            return std::nullopt;
        }
        const auto program_hash =
            deferred_unit ? ReadDeferredProgramHash(loaded.symbols.isolate_data,
                                                    loaded.symbols.isolate_data_size, *header)
                          : std::optional<uint32_t>{};
        if (deferred_unit && !program_hash.has_value()) {
            if (error != nullptr) *error = "loaded deferred Dart program hash is malformed";
            return std::nullopt;
        }
        return build_source(*header, program_hash, loaded.symbols.isolate_instructions_va,
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
    const FileSnapshotLookup dynamic =
        FindFileDynamicSnapshotSymbols(*bytes, data_symbol, instructions_symbol);
    if (dynamic.status == ElfDynamicLookupStatus::kFound) {
        const auto header = ReadSnapshotHeader(*bytes, dynamic.isolate_data);
        if (!header.has_value()) {
            if (error != nullptr) *error = "Dart isolate snapshot header is not recognized";
            return std::nullopt;
        }
        const auto program_hash =
            deferred_unit ? ReadDeferredProgramHash(*bytes, dynamic.isolate_data, *header)
                          : std::optional<uint32_t>{};
        if (deferred_unit && !program_hash.has_value()) {
            if (error != nullptr) *error = "deferred Dart program hash is malformed";
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
        return build_source(*header, program_hash, dynamic.isolate_instructions.value,
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
    std::vector<ElfProgramHeaderView> headers;
    if (!ParseElf64ProgramHeaders(*bytes, &headers)) {
        if (error != nullptr) *error = "Flutter app ELF program headers are malformed";
        return std::nullopt;
    }
    const auto isolate_data = FindElfSectionSymbol(*bytes, headers, data_symbol);
    const auto isolate_instr = FindElfSectionSymbol(*bytes, headers, instructions_symbol);
    if (isolate_data.status == ElfSectionLookupStatus::kMalformed ||
        isolate_instr.status == ElfSectionLookupStatus::kMalformed) {
        if (error != nullptr) *error = "Flutter app section-table snapshot symbols are malformed";
        return std::nullopt;
    }
    if (isolate_data.status != ElfSectionLookupStatus::kFound ||
        isolate_instr.status != ElfSectionLookupStatus::kFound || isolate_instr.symbol.size == 0) {
        if (error != nullptr) *error = "loaded app module has no Dart isolate snapshot symbols";
        return std::nullopt;
    }
    if (!SnapshotSymbolMatchesContract(isolate_data.symbol, headers, PF_R, false) ||
        !SnapshotSymbolMatchesContract(isolate_instr.symbol, headers, PF_R | PF_X, false)) {
        if (error != nullptr) {
            *error = "Flutter app section-table snapshot symbols lack required PT_LOAD flags";
        }
        return std::nullopt;
    }
    const auto header = ReadSnapshotHeader(*bytes, isolate_data.symbol);
    if (!header.has_value()) {
        if (error != nullptr) *error = "Dart isolate snapshot header is not recognized";
        return std::nullopt;
    }
    const auto program_hash = deferred_unit
                                  ? ReadDeferredProgramHash(*bytes, isolate_data.symbol, *header)
                                  : std::optional<uint32_t>{};
    if (deferred_unit && !program_hash.has_value()) {
        if (error != nullptr) *error = "deferred Dart program hash is malformed";
        return std::nullopt;
    }
    if (isolate_instr.symbol.value > UINTPTR_MAX - module.load_bias) {
        if (error != nullptr) *error = "Dart isolate snapshot instructions VA overflows runtime";
        return std::nullopt;
    }
    return build_source(*header, program_hash, isolate_instr.symbol.value,
                        isolate_instr.symbol.size, module.load_bias + isolate_instr.symbol.value);
}

}  // namespace

std::optional<FlutterSnapshotSource> DiscoverFlutterSnapshot(const ModuleImage& module,
                                                             std::string* error) {
    return DiscoverFlutterSnapshotWithSymbols(module, "_kDartIsolateSnapshotData",
                                              "_kDartIsolateSnapshotInstructions", false, error);
}

std::optional<FlutterSnapshotSource> DiscoverDeferredFlutterSnapshot(const ModuleImage& module,
                                                                     std::string* error) {
    return DiscoverFlutterSnapshotWithSymbols(module, "_kDartIsolateSnapshotData",
                                              "_kDartIsolateSnapshotInstructions", true, error);
}

}  // namespace dartplant
