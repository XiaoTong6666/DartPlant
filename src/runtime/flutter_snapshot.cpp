#include <elf.h>
#include <link.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

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
};

uintptr_t DynamicPointer(uintptr_t load_bias, Elf64_Addr pointer) {
    const uintptr_t value = static_cast<uintptr_t>(pointer);
    return value < load_bias ? load_bias + value : value;
}

int FindLoadedSnapshotSymbols(dl_phdr_info* info, size_t, void* opaque) {
    auto* lookup = static_cast<LoadedSnapshotLookup*>(opaque);
    if (info == nullptr || lookup == nullptr || lookup->module == nullptr ||
        static_cast<uintptr_t>(info->dlpi_addr) != lookup->module->load_bias) {
        return 0;
    }
    const Elf64_Dyn* dynamic = nullptr;
    size_t dynamic_count = 0;
    for (Elf64_Half index = 0; index < info->dlpi_phnum; ++index) {
        const Elf64_Phdr& phdr = info->dlpi_phdr[index];
        if (phdr.p_type == PT_DYNAMIC) {
            dynamic = reinterpret_cast<const Elf64_Dyn*>(info->dlpi_addr + phdr.p_vaddr);
            dynamic_count = phdr.p_memsz / sizeof(Elf64_Dyn);
            break;
        }
    }
    if (dynamic == nullptr) return 1;

    const Elf64_Sym* symbols = nullptr;
    const char* strings = nullptr;
    const uint32_t* hash = nullptr;
    size_t symbol_size = sizeof(Elf64_Sym);
    for (size_t index = 0; index < dynamic_count && dynamic[index].d_tag != DT_NULL; ++index) {
        switch (dynamic[index].d_tag) {
        case DT_SYMTAB:
            symbols = reinterpret_cast<const Elf64_Sym*>(
                DynamicPointer(info->dlpi_addr, dynamic[index].d_un.d_ptr));
            break;
        case DT_STRTAB:
            strings = reinterpret_cast<const char*>(
                DynamicPointer(info->dlpi_addr, dynamic[index].d_un.d_ptr));
            break;
        case DT_HASH:
            hash = reinterpret_cast<const uint32_t*>(
                DynamicPointer(info->dlpi_addr, dynamic[index].d_un.d_ptr));
            break;
        case DT_SYMENT:
            symbol_size = dynamic[index].d_un.d_val;
            break;
        default:
            break;
        }
    }
    if (symbols == nullptr || strings == nullptr || hash == nullptr ||
        symbol_size != sizeof(Elf64_Sym)) {
        return 1;
    }
    const uint32_t symbol_count = hash[1];
    for (uint32_t index = 0; index < symbol_count; ++index) {
        const Elf64_Sym& symbol = symbols[index];
        if (symbol.st_name == 0 || symbol.st_value == 0) continue;
        const char* name = strings + symbol.st_name;
        if (strcmp(name, "_kDartIsolateSnapshotData") == 0) {
            lookup->symbols.isolate_data = info->dlpi_addr + symbol.st_value;
            lookup->symbols.isolate_data_size = symbol.st_size;
        } else if (strcmp(name, "_kDartIsolateSnapshotInstructions") == 0) {
            lookup->symbols.isolate_instructions = info->dlpi_addr + symbol.st_value;
            lookup->symbols.isolate_instructions_size = symbol.st_size;
            lookup->symbols.isolate_instructions_va = symbol.st_value;
        }
    }
    return 1;
}

std::optional<std::vector<uint8_t>> ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
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

std::optional<std::pair<std::string, std::string>> ReadSnapshotHeader(
    const std::vector<uint8_t>& bytes, const ElfSymbol& symbol) {
    constexpr uint8_t kMagic[] = {0xf5, 0xf5, 0xdc, 0xdc};
    constexpr size_t kHeaderSize = 20;
    constexpr size_t kHashSize = 32;
    if (symbol.file_offset > bytes.size() || symbol.size < kHeaderSize + kHashSize ||
        symbol.file_offset + symbol.size > bytes.size()) {
        return std::nullopt;
    }
    const size_t start = static_cast<size_t>(symbol.file_offset);
    if (memcmp(bytes.data() + start, kMagic, sizeof(kMagic)) != 0) return std::nullopt;
    const auto* hash = bytes.data() + start + kHeaderSize;
    if (!std::all_of(hash, hash + kHashSize, [](uint8_t value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                   (value >= 'A' && value <= 'F');
        })) {
        return std::nullopt;
    }
    const auto* features = reinterpret_cast<const char*>(hash + kHashSize);
    const size_t remaining = symbol.size - kHeaderSize - kHashSize;
    const size_t length = strnlen(features, remaining);
    if (length == remaining) return std::nullopt;
    return std::make_pair(std::string(reinterpret_cast<const char*>(hash), kHashSize),
                          std::string(features, length));
}

std::optional<std::pair<std::string, std::string>> ReadSnapshotHeader(uintptr_t address,
                                                                      uint64_t size) {
    constexpr uint8_t kMagic[] = {0xf5, 0xf5, 0xdc, 0xdc};
    constexpr size_t kHeaderSize = 20;
    constexpr size_t kHashSize = 32;
    if (address == 0 || size < kHeaderSize + kHashSize) return std::nullopt;
    const auto* bytes = reinterpret_cast<const uint8_t*>(address);
    if (memcmp(bytes, kMagic, sizeof(kMagic)) != 0) return std::nullopt;
    const auto* hash = bytes + kHeaderSize;
    if (!std::all_of(hash, hash + kHashSize, [](uint8_t value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                   (value >= 'A' && value <= 'F');
        })) {
        return std::nullopt;
    }
    const char* features = reinterpret_cast<const char*>(hash + kHashSize);
    const size_t remaining = size - kHeaderSize - kHashSize;
    const size_t length = strnlen(features, remaining);
    if (length == remaining) return std::nullopt;
    return std::make_pair(std::string(reinterpret_cast<const char*>(hash), kHashSize),
                          std::string(features, length));
}

bool HasFeature(std::string_view features, std::string_view feature) {
    return features.find(feature) != std::string_view::npos;
}

}  // namespace

std::optional<std::string> SelectFlutterSnapshotProfile(std::string_view features) {
    const bool arm64 = HasFeature(features, "arm64");
    const bool product = HasFeature(features, "product");
    const bool profile = HasFeature(features, "profile");
    const bool compressed = HasFeature(features, "compressed-pointers") &&
                            !HasFeature(features, "no-compressed-pointers");
    if (!arm64 || (product == profile)) return std::nullopt;
    if (product && compressed) return "flutter-arm64-product-compressed";
    if (product && !compressed) return "flutter-arm64-product-uncompressed";
    if (profile && compressed) return "flutter-arm64-profile-compressed";
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

std::optional<FlutterSnapshotSource> DiscoverFlutterSnapshot(const ModuleImage& module,
                                                             std::string* error) {
    const auto bytes = ReadFile(module.path);
    if (!bytes.has_value()) {
        LoadedSnapshotLookup lookup{.module = &module};
        dl_iterate_phdr(FindLoadedSnapshotSymbols, &lookup);
        const auto& symbols = lookup.symbols;
        if (symbols.isolate_data == 0 || symbols.isolate_data_size == 0 ||
            symbols.isolate_instructions == 0 || symbols.isolate_instructions_size == 0) {
            if (error != nullptr)
                *error = "cannot read loaded Flutter app module or its dynamic snapshot symbols";
            return std::nullopt;
        }
        const auto header = ReadSnapshotHeader(symbols.isolate_data, symbols.isolate_data_size);
        if (!header.has_value()) {
            if (error != nullptr) *error = "loaded Dart isolate snapshot header is not recognized";
            return std::nullopt;
        }
        FlutterSnapshotSource source;
        source.module_name = module.name;
        source.module_path = module.path;
        source.module_build_id = module.build_id;
        source.snapshot_hash = header->first;
        source.snapshot_features = header->second;
        const auto profile = SelectFlutterSnapshotProfile(source.snapshot_features);
        if (!profile.has_value()) {
            if (error != nullptr) *error = "Flutter snapshot feature profile is unsupported";
            return std::nullopt;
        }
        source.profile_name = *profile;
        source.isolate_instructions_va = symbols.isolate_instructions_va;
        source.isolate_instructions_size = symbols.isolate_instructions_size;
        source.isolate_instructions_runtime = symbols.isolate_instructions;
        source.compressed_pointers =
            HasFeature(source.snapshot_features, "compressed-pointers") &&
            !HasFeature(source.snapshot_features, "no-compressed-pointers");
        return source;
    }
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
    FlutterSnapshotSource source;
    source.module_name = module.name;
    source.module_path = module.path;
    source.module_build_id = module.build_id;
    source.snapshot_hash = header->first;
    source.snapshot_features = header->second;
    const auto profile = SelectFlutterSnapshotProfile(source.snapshot_features);
    if (!profile.has_value()) {
        if (error != nullptr) *error = "Flutter snapshot feature profile is unsupported";
        return std::nullopt;
    }
    source.profile_name = *profile;
    source.isolate_instructions_va = isolate_instr->value;
    source.isolate_instructions_size = isolate_instr->size;
    source.isolate_instructions_runtime = module.load_bias + isolate_instr->value;
    source.compressed_pointers = HasFeature(source.snapshot_features, "compressed-pointers") &&
                                 !HasFeature(source.snapshot_features, "no-compressed-pointers");
    return source;
}

}  // namespace dartplant
