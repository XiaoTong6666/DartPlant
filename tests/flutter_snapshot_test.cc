#include <elf.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "elf/dynamic_view.h"
#include "runtime/flutter_snapshot_internal.h"
#include "test_runner.h"

namespace {

std::vector<uint8_t> MakeFullAotSnapshotHeader(std::string_view features,
                                               uint64_t declared_length = 0) {
    constexpr size_t kHeaderSize = 20;
    constexpr char kHash[] = "0123456789abcdef0123456789abcdef";
    const size_t minimum = kHeaderSize + 32 + features.size() + 1;
    const size_t total = declared_length == 0 ? minimum : static_cast<size_t>(declared_length);
    std::vector<uint8_t> bytes(total, 0);
    const int32_t magic = static_cast<int32_t>(0xdcdcf5f5U);
    const int64_t stored_length = static_cast<int64_t>(total - sizeof(int32_t));
    const int64_t kind = 3;
    std::memcpy(bytes.data(), &magic, sizeof(magic));
    std::memcpy(bytes.data() + 4, &stored_length, sizeof(stored_length));
    std::memcpy(bytes.data() + 12, &kind, sizeof(kind));
    std::memcpy(bytes.data() + kHeaderSize, kHash, 32);
    if (minimum <= bytes.size()) {
        std::memcpy(bytes.data() + kHeaderSize + 32, features.data(), features.size());
        bytes[kHeaderSize + 32 + features.size()] = 0;
    }
    return bytes;
}

template <typename T>
void WriteAt(std::vector<uint8_t>* bytes, size_t offset, const T& value) {
    EXPECT_TRUE(bytes != nullptr);
    EXPECT_TRUE(offset <= bytes->size());
    EXPECT_TRUE(sizeof(T) <= bytes->size() - offset);
    std::memcpy(bytes->data() + offset, &value, sizeof(T));
}

std::vector<uint8_t> MakeSectionSymbolElf(uint32_t name_offset = 1, bool terminate_name = true) {
    std::vector<uint8_t> bytes(0x400, 0);
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_shoff = 0x100;
    header.e_shentsize = sizeof(Elf64_Shdr);
    header.e_shnum = 4;
    WriteAt(&bytes, 0, header);

    Elf64_Shdr symtab{};
    symtab.sh_type = SHT_DYNSYM;
    symtab.sh_offset = 0x200;
    symtab.sh_size = 2 * sizeof(Elf64_Sym);
    symtab.sh_link = 2;
    symtab.sh_entsize = sizeof(Elf64_Sym);
    WriteAt(&bytes, 0x100 + sizeof(Elf64_Shdr), symtab);

    Elf64_Shdr strings{};
    strings.sh_type = SHT_STRTAB;
    strings.sh_offset = 0x280;
    strings.sh_size = 8;
    WriteAt(&bytes, 0x100 + 2 * sizeof(Elf64_Shdr), strings);

    Elf64_Shdr data{};
    data.sh_type = SHT_PROGBITS;
    data.sh_offset = 0x300;
    data.sh_addr = 0x5000;
    data.sh_size = 0x40;
    data.sh_flags = SHF_ALLOC;
    WriteAt(&bytes, 0x100 + 3 * sizeof(Elf64_Shdr), data);

    Elf64_Sym symbol{};
    symbol.st_name = name_offset;
    symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
    symbol.st_shndx = 3;
    symbol.st_value = 0x5000;
    symbol.st_size = 4;
    WriteAt(&bytes, 0x200 + sizeof(Elf64_Sym), symbol);

    if (terminate_name) {
        constexpr char kNames[] = "\0target\0";
        std::memcpy(bytes.data() + 0x280, kNames, sizeof(kNames) - 1);
    } else {
        constexpr char kNoTerminator[] = "\0targetX";
        std::memcpy(bytes.data() + 0x280, kNoTerminator, sizeof(kNoTerminator) - 1);
    }
    return bytes;
}

std::array<dartplant::ElfProgramHeaderView, 1> SectionSymbolProgramHeaders(
    uint64_t offset = 0x300) {
    return {dartplant::ElfProgramHeaderView{
        .type = PT_LOAD,
        .flags = PF_R,
        .offset = offset,
        .virtual_address = 0x5000,
        .file_size = 0x40,
        .memory_size = 0x40,
    }};
}

enum class DynamicHashStyle {
    kSysv,
    kGnu,
};

constexpr uint64_t kSyntheticDataVa = 0x1000;
constexpr uint64_t kSyntheticInstructionsVa = 0x7000;
constexpr uint64_t kSyntheticInstructionsSize = 0x100;
constexpr uintptr_t kSyntheticLoadBias = static_cast<uintptr_t>(0x123450000000ULL);

uint32_t GnuHash(std::string_view name) {
    uint32_t hash = 5381;
    for (const unsigned char value : name) hash = hash * 33U + value;
    return hash;
}

std::vector<uint8_t> MakeDynamicSnapshotElf(
    DynamicHashStyle hash_style, bool duplicate_dynamic = false, bool add_valid_sections = false,
    bool executable_instructions = true,
    std::optional<uint32_t> deferred_program_hash = std::nullopt) {
    constexpr size_t kDynamicOffset = 0x400;
    constexpr uint64_t kDynamicVa = 0x400;
    constexpr size_t kSymtabOffset = 0x600;
    constexpr uint64_t kSymtabVa = 0x600;
    constexpr size_t kStrtabOffset = 0x700;
    constexpr uint64_t kStrtabVa = 0x700;
    constexpr size_t kHashOffset = 0x800;
    constexpr uint64_t kHashVa = 0x800;
    constexpr size_t kDataOffset = 0x1000;
    constexpr size_t kInstructionsOffset = 0x2000;
    constexpr size_t kSectionsOffset = 0x3000;
    constexpr std::string_view kDataName = "_kDartIsolateSnapshotData";
    constexpr std::string_view kInstructionsName = "_kDartIsolateSnapshotInstructions";

    auto snapshot =
        MakeFullAotSnapshotHeader("product arm64 android compressed-pointers null-safety");
    if (deferred_program_hash.has_value()) {
        const size_t payload_offset = snapshot.size();
        snapshot.resize(snapshot.size() + sizeof(uint32_t));
        WriteAt(&snapshot, payload_offset, *deferred_program_hash);
        const int64_t stored_length = static_cast<int64_t>(snapshot.size() - sizeof(int32_t));
        WriteAt(&snapshot, sizeof(int32_t), stored_length);
    }
    std::vector<uint8_t> bytes(0x4000, 0);

    const Elf64_Half base_phnum = 5;  // R metadata/data, three RX loads, PT_DYNAMIC.
    const Elf64_Half phnum = base_phnum + (duplicate_dynamic ? 1 : 0);
    Elf64_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_DYN;
    header.e_machine = EM_AARCH64;
    header.e_version = EV_CURRENT;
    header.e_phoff = sizeof(Elf64_Ehdr);
    header.e_phentsize = sizeof(Elf64_Phdr);
    header.e_phnum = phnum;
    if (add_valid_sections) {
        header.e_shoff = kSectionsOffset;
        header.e_shentsize = sizeof(Elf64_Shdr);
        header.e_shnum = 5;
    }
    WriteAt(&bytes, 0, header);

    const auto write_phdr = [&](size_t index, const Elf64_Phdr& phdr) {
        WriteAt(&bytes, sizeof(Elf64_Ehdr) + index * sizeof(Elf64_Phdr), phdr);
    };

    Elf64_Phdr metadata{};
    metadata.p_type = PT_LOAD;
    metadata.p_flags = PF_R;
    metadata.p_offset = 0;
    metadata.p_vaddr = 0;
    metadata.p_filesz = 0x1800;
    metadata.p_memsz = 0x1800;
    write_phdr(0, metadata);

    // Keep multiple executable loads in the corpus so discovery cannot assume
    // the isolate instructions live in the first RX segment.
    Elf64_Phdr first_rx{};
    first_rx.p_type = PT_LOAD;
    first_rx.p_flags = PF_R | PF_X;
    first_rx.p_offset = 0x1800;
    first_rx.p_vaddr = 0x3000;
    first_rx.p_filesz = 0x80;
    first_rx.p_memsz = 0x80;
    write_phdr(1, first_rx);

    Elf64_Phdr second_rx = first_rx;
    second_rx.p_offset = 0x1900;
    second_rx.p_vaddr = 0x5000;
    write_phdr(2, second_rx);

    Elf64_Phdr instructions{};
    instructions.p_type = PT_LOAD;
    instructions.p_flags = PF_R | (executable_instructions ? PF_X : 0);
    instructions.p_offset = kInstructionsOffset;
    instructions.p_vaddr = kSyntheticInstructionsVa;
    instructions.p_filesz = 0x400;
    instructions.p_memsz = 0x400;
    write_phdr(3, instructions);

    constexpr size_t kDynamicEntries = 6;
    const std::array dynamic = {
        Elf64_Dyn{.d_tag = DT_SYMTAB, .d_un = {.d_ptr = kSymtabVa}},
        Elf64_Dyn{.d_tag = DT_STRTAB, .d_un = {.d_ptr = kStrtabVa}},
        Elf64_Dyn{.d_tag = DT_STRSZ,
                  .d_un = {.d_val = 1 + kDataName.size() + 1 + kInstructionsName.size() + 1}},
        Elf64_Dyn{.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
        Elf64_Dyn{.d_tag = hash_style == DynamicHashStyle::kSysv ? DT_HASH : DT_GNU_HASH,
                  .d_un = {.d_ptr = kHashVa}},
        Elf64_Dyn{.d_tag = DT_NULL, .d_un = {.d_val = 0}},
    };
    static_assert(dynamic.size() == kDynamicEntries);
    std::memcpy(bytes.data() + kDynamicOffset, dynamic.data(), sizeof(dynamic));

    Elf64_Phdr dynamic_phdr{};
    dynamic_phdr.p_type = PT_DYNAMIC;
    dynamic_phdr.p_flags = PF_R;
    dynamic_phdr.p_offset = kDynamicOffset;
    dynamic_phdr.p_vaddr = kDynamicVa;
    dynamic_phdr.p_filesz = sizeof(dynamic);
    dynamic_phdr.p_memsz = sizeof(dynamic);
    write_phdr(4, dynamic_phdr);
    if (duplicate_dynamic) write_phdr(5, dynamic_phdr);

    const uint32_t data_name_offset = 1;
    const uint32_t instructions_name_offset =
        data_name_offset + static_cast<uint32_t>(kDataName.size()) + 1;
    bytes[kStrtabOffset] = 0;
    std::memcpy(bytes.data() + kStrtabOffset + data_name_offset, kDataName.data(),
                kDataName.size());
    bytes[kStrtabOffset + data_name_offset + kDataName.size()] = 0;
    std::memcpy(bytes.data() + kStrtabOffset + instructions_name_offset, kInstructionsName.data(),
                kInstructionsName.size());
    bytes[kStrtabOffset + instructions_name_offset + kInstructionsName.size()] = 0;

    Elf64_Sym data_symbol{};
    data_symbol.st_name = data_name_offset;
    data_symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
    data_symbol.st_shndx = 3;
    data_symbol.st_value = kSyntheticDataVa;
    data_symbol.st_size = snapshot.size();
    WriteAt(&bytes, kSymtabOffset + sizeof(Elf64_Sym), data_symbol);

    Elf64_Sym instructions_symbol{};
    instructions_symbol.st_name = instructions_name_offset;
    instructions_symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
    instructions_symbol.st_shndx = 4;
    instructions_symbol.st_value = kSyntheticInstructionsVa;
    instructions_symbol.st_size = kSyntheticInstructionsSize;
    WriteAt(&bytes, kSymtabOffset + 2 * sizeof(Elf64_Sym), instructions_symbol);

    if (hash_style == DynamicHashStyle::kSysv) {
        const std::array<uint32_t, 6> hash = {
            1,  // nbucket
            3,  // nchain
            1,  // bucket[0]
            0,  // chain[0]
            2,  // chain[1]
            0,  // chain[2]
        };
        std::memcpy(bytes.data() + kHashOffset, hash.data(), sizeof(hash));
    } else {
        constexpr uint32_t kBloomShift = 5;
        const uint32_t data_hash = GnuHash(kDataName);
        const uint32_t instructions_hash = GnuHash(kInstructionsName);
        Elf64_Addr bloom = 0;
        const auto add_bloom = [&](uint32_t hash) {
            bloom |= Elf64_Addr{1} << (hash % 64);
            bloom |= Elf64_Addr{1} << ((hash >> kBloomShift) % 64);
        };
        add_bloom(data_hash);
        add_bloom(instructions_hash);
        const std::array<uint32_t, 4> gnu_header = {1, 1, 1, kBloomShift};
        std::memcpy(bytes.data() + kHashOffset, gnu_header.data(), sizeof(gnu_header));
        WriteAt(&bytes, kHashOffset + sizeof(gnu_header), bloom);
        const uint32_t bucket = 1;
        WriteAt(&bytes, kHashOffset + sizeof(gnu_header) + sizeof(bloom), bucket);
        const std::array<uint32_t, 2> chains = {data_hash & ~1U, instructions_hash | 1U};
        std::memcpy(
            bytes.data() + kHashOffset + sizeof(gnu_header) + sizeof(bloom) + sizeof(bucket),
            chains.data(), sizeof(chains));
    }

    std::memcpy(bytes.data() + kDataOffset, snapshot.data(), snapshot.size());
    for (size_t index = 0; index < kSyntheticInstructionsSize; ++index) {
        bytes[kInstructionsOffset + index] = static_cast<uint8_t>(0xa0 + (index & 0xf));
    }

    if (add_valid_sections) {
        Elf64_Shdr dynsym{};
        dynsym.sh_type = SHT_DYNSYM;
        dynsym.sh_offset = kSymtabOffset;
        dynsym.sh_addr = kSymtabVa;
        dynsym.sh_size = 3 * sizeof(Elf64_Sym);
        dynsym.sh_link = 2;
        dynsym.sh_entsize = sizeof(Elf64_Sym);
        WriteAt(&bytes, kSectionsOffset + sizeof(Elf64_Shdr), dynsym);

        Elf64_Shdr strtab{};
        strtab.sh_type = SHT_STRTAB;
        strtab.sh_offset = kStrtabOffset;
        strtab.sh_addr = kStrtabVa;
        strtab.sh_size = dynamic[2].d_un.d_val;
        WriteAt(&bytes, kSectionsOffset + 2 * sizeof(Elf64_Shdr), strtab);

        Elf64_Shdr data{};
        data.sh_type = SHT_PROGBITS;
        data.sh_flags = SHF_ALLOC;
        data.sh_offset = kDataOffset;
        data.sh_addr = kSyntheticDataVa;
        data.sh_size = snapshot.size();
        WriteAt(&bytes, kSectionsOffset + 3 * sizeof(Elf64_Shdr), data);

        Elf64_Shdr text{};
        text.sh_type = SHT_PROGBITS;
        text.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
        text.sh_offset = kInstructionsOffset;
        text.sh_addr = kSyntheticInstructionsVa;
        text.sh_size = 0x400;
        WriteAt(&bytes, kSectionsOffset + 4 * sizeof(Elf64_Shdr), text);
    }

    return bytes;
}

class TemporaryElfFile {
public:
    explicit TemporaryElfFile(const std::vector<uint8_t>& bytes) {
        std::array<char, 64> pattern{};
        const int written = std::snprintf(pattern.data(), pattern.size(),
                                          "/tmp/dartplant-snapshot-%d-XXXXXX", getpid());
        EXPECT_TRUE(written > 0 && static_cast<size_t>(written) < pattern.size());
        const int fd = mkstemp(pattern.data());
        EXPECT_TRUE(fd >= 0);
        path_ = pattern.data();
        size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = write(fd, bytes.data() + offset, bytes.size() - offset);
            EXPECT_TRUE(count > 0);
            offset += static_cast<size_t>(count);
        }
        EXPECT_EQ(0, close(fd));
    }

    ~TemporaryElfFile() {
        if (!path_.empty()) std::remove(path_.c_str());
    }

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

dartplant::ModuleImage MakeSyntheticModule(const std::vector<uint8_t>& bytes,
                                           const std::string& path) {
    std::vector<dartplant::ElfProgramHeaderView> headers;
    EXPECT_TRUE(dartplant::ParseElf64ProgramHeaders(bytes, &headers));
    dartplant::ModuleImage module;
    EXPECT_TRUE(
        dartplant::BuildModuleImageFromProgramHeaders(path, kSyntheticLoadBias, headers, &module));
    module.build_id = "synthetic-build-id";
    return module;
}

}  // namespace

TEST_CASE(FlutterSnapshotProfileSelectsCurrentArm64ProductLayout) {
    const auto profile = dartplant::SelectFlutterSnapshotProfile(
        "product arm64 android compressed-pointers CID_SHIFT1");
    EXPECT_TRUE(profile.has_value());
    EXPECT_EQ(std::string("flutter-arm64-product-compressed"), *profile);
}

TEST_CASE(FlutterProfileSnapshotMapsDartReleaseFeatureToNonProductLayout) {
    const auto profile = dartplant::SelectFlutterSnapshotProfile(
        "release arm64 android compressed-pointers null-safety");
    EXPECT_TRUE(profile.has_value());
    EXPECT_EQ(std::string("flutter-arm64-profile-compressed"), *profile);
    EXPECT_FALSE(
        dartplant::SelectFlutterSnapshotProfile("profile arm64 android compressed-pointers")
            .has_value());
}

TEST_CASE(FlutterSnapshotProfileRejectsUnknownLayout) {
    const auto profile = dartplant::SelectFlutterSnapshotProfile("arm64 experimental-layout");
    EXPECT_FALSE(profile.has_value());
}

TEST_CASE(FlutterSnapshotOffsetResolvesOnlyWithinProvenIsolateInstructionsRegion) {
    dartplant::ModuleImage module;
    module.name = "libapp.so";
    module.path = "/data/app/example/libapp.so";
    module.build_id = "build";
    module.load_bias = 0x100000;
    module.executable_ranges.push_back({
        .start = 0x107000,
        .end = 0x109000,
        .file_offset = 0x7000,
        .virtual_address = 0x7000,
        .file_size = 0x2000,
    });
    dartplant::FlutterSnapshotSource snapshot;
    snapshot.module_name = module.name;
    snapshot.module_path = module.path;
    snapshot.module_build_id = module.build_id;
    snapshot.isolate_instructions_va = 0x7000;
    snapshot.isolate_instructions_size = 0x2000;
    snapshot.isolate_instructions_runtime = 0x107000;

    const auto resolved = snapshot.ResolveInstructionOffset(module, 0x180);
    EXPECT_TRUE(resolved.has_value());
    EXPECT_EQ(0x107180U, *resolved);
    EXPECT_TRUE(snapshot.ResolveInstructionRange(module, 0x1f00, 0x100).has_value());
    EXPECT_FALSE(snapshot.ResolveInstructionRange(module, 0x1ffe, 4).has_value());
    EXPECT_FALSE(snapshot.ResolveInstructionRange(module, 0x180, 0).has_value());
    EXPECT_FALSE(snapshot.ResolveInstructionOffset(module, 0x2000).has_value());
}

TEST_CASE(DartSnapshotHeaderValidatesDeclaredLengthAndFullAotKind) {
    const auto bytes =
        MakeFullAotSnapshotHeader("product arm64 android compressed-pointers null-safety");
    const auto parsed = dartplant::ParseDartSnapshotHeader(bytes);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(bytes.size(), parsed->declared_length);
    EXPECT_EQ(3U, parsed->kind);
    EXPECT_EQ(std::string("0123456789abcdef0123456789abcdef"), parsed->snapshot_hash);
    EXPECT_EQ(std::string("product arm64 android compressed-pointers null-safety"),
              parsed->features);
    EXPECT_EQ(bytes.size(), parsed->payload_offset);
}

TEST_CASE(DartDeferredProgramHashStartsImmediatelyAfterVersionAndFeatures) {
    auto bytes = MakeFullAotSnapshotHeader("product arm64 android compressed-pointers null-safety");
    const size_t program_hash_offset = bytes.size();
    bytes.resize(bytes.size() + sizeof(uint32_t));
    constexpr uint32_t kProgramHash = 0x1a30145f;
    WriteAt(&bytes, program_hash_offset, kProgramHash);
    const int64_t stored_length = static_cast<int64_t>(bytes.size() - sizeof(int32_t));
    WriteAt(&bytes, sizeof(int32_t), stored_length);

    const auto parsed = dartplant::ParseDartSnapshotHeader(bytes);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(program_hash_offset, parsed->payload_offset);
    EXPECT_EQ(kProgramHash, dartplant::ParseDartDeferredProgramHash(bytes, *parsed).value_or(0));

    bytes.resize(program_hash_offset + sizeof(uint32_t) - 1);
    EXPECT_FALSE(dartplant::ParseDartDeferredProgramHash(bytes, *parsed).has_value());
}

TEST_CASE(DartSnapshotHeaderRejectsWrongKindAndLengthBeyondSymbol) {
    auto bytes = MakeFullAotSnapshotHeader("product arm64 android compressed-pointers");
    const int64_t wrong_kind = 2;
    std::memcpy(bytes.data() + 12, &wrong_kind, sizeof(wrong_kind));
    EXPECT_FALSE(dartplant::ParseDartSnapshotHeader(bytes).has_value());

    bytes = MakeFullAotSnapshotHeader("product arm64 android compressed-pointers");
    const int64_t oversized = static_cast<int64_t>(bytes.size() + 0x100 - sizeof(int32_t));
    std::memcpy(bytes.data() + 4, &oversized, sizeof(oversized));
    EXPECT_FALSE(dartplant::ParseDartSnapshotHeader(bytes).has_value());
}

TEST_CASE(DartSnapshotHeaderDoesNotSearchForFeaturesTerminatorPastDeclaredLength) {
    auto bytes = MakeFullAotSnapshotHeader("product arm64 android compressed-pointers");
    const size_t feature_start = 20 + 32;
    const int64_t shortened = static_cast<int64_t>(feature_start + 8 - sizeof(int32_t));
    std::memcpy(bytes.data() + 4, &shortened, sizeof(shortened));
    EXPECT_FALSE(dartplant::ParseDartSnapshotHeader(bytes).has_value());
}

TEST_CASE(FlutterSectionFallbackUsesBoundedStringTableLookup) {
    const auto bytes = MakeSectionSymbolElf();
    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kFound),
              static_cast<uint32_t>(result.status));
    EXPECT_EQ(0x5000U, result.symbol.value);
    EXPECT_EQ(0x300U, result.symbol.file_offset);
}

TEST_CASE(FlutterSectionFallbackRejectsExtendedSectionNumberingUntilSupported) {
    auto bytes = MakeSectionSymbolElf();
    Elf64_Ehdr header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    // gABI extended numbering: e_shnum==0 with a non-zero section table
    // means the actual count lives in section-header[0].sh_size. The fallback
    // intentionally does not implement that producer shape yet.
    header.e_shnum = 0;
    WriteAt(&bytes, 0, header);
    Elf64_Shdr section0{};
    std::memcpy(&section0, bytes.data() + 0x100, sizeof(section0));
    section0.sh_size = 4;
    WriteAt(&bytes, 0x100, section0);

    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(FlutterSectionFallbackRejectsShnXindexUntilSymtabShndxIsSupported) {
    auto bytes = MakeSectionSymbolElf();
    Elf64_Sym symbol{};
    std::memcpy(&symbol, bytes.data() + 0x200 + sizeof(Elf64_Sym), sizeof(symbol));
    symbol.st_shndx = SHN_XINDEX;
    WriteAt(&bytes, 0x200 + sizeof(Elf64_Sym), symbol);

    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(FlutterSectionFallbackRejectsSectionMappingThatConflictsWithPtLoad) {
    const auto bytes = MakeSectionSymbolElf();
    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(0x180), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(FlutterSectionFallbackRequiresAllocatedOwnerSection) {
    auto bytes = MakeSectionSymbolElf();
    Elf64_Shdr owner{};
    constexpr size_t kOwnerOffset = 0x100 + 3 * sizeof(Elf64_Shdr);
    std::memcpy(&owner, bytes.data() + kOwnerOffset, sizeof(owner));
    owner.sh_flags = 0;
    WriteAt(&bytes, kOwnerOffset, owner);
    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(FlutterSectionFallbackDoesNotRescueMissingDynsymSymbol) {
    auto bytes = MakeSectionSymbolElf();

    Elf64_Shdr static_symbols{};
    static_symbols.sh_type = SHT_SYMTAB;
    static_symbols.sh_offset = 0x200;
    static_symbols.sh_size = 2 * sizeof(Elf64_Sym);
    static_symbols.sh_link = 2;
    static_symbols.sh_entsize = sizeof(Elf64_Sym);
    WriteAt(&bytes, 0x100, static_symbols);

    Elf64_Shdr dynamic_symbols{};
    std::memcpy(&dynamic_symbols, bytes.data() + 0x100 + sizeof(Elf64_Shdr),
                sizeof(dynamic_symbols));
    dynamic_symbols.sh_offset = 0x340;
    dynamic_symbols.sh_size = sizeof(Elf64_Sym);
    WriteAt(&bytes, 0x100 + sizeof(Elf64_Shdr), dynamic_symbols);
    WriteAt(&bytes, 0x340, Elf64_Sym{});

    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kNotFound),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(FlutterSnapshotSymbolContractRequiresGlobalObjectAndSegmentFlags) {
    const auto headers = SectionSymbolProgramHeaders();
    Elf64_Sym base{};
    base.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
    base.st_shndx = 3;
    base.st_value = 0x5000;
    base.st_size = 4;
    const auto to_dynamic = [](const Elf64_Sym& symbol) {
        return dartplant::ElfDynamicSymbol{
            .value = symbol.st_value,
            .size = symbol.st_size,
            .info = symbol.st_info,
            .other = symbol.st_other,
            .section_index = symbol.st_shndx,
        };
    };
    const auto to_section = [](const Elf64_Sym& symbol) {
        return dartplant::ElfSectionSymbol{
            .value = symbol.st_value,
            .size = symbol.st_size,
            .info = symbol.st_info,
            .other = symbol.st_other,
            .section_index = symbol.st_shndx,
        };
    };

    EXPECT_TRUE(dartplant::SnapshotSymbolMatchesContract(to_dynamic(base), headers, PF_R, false));
    EXPECT_TRUE(dartplant::SnapshotSymbolMatchesContract(to_section(base), headers, PF_R, false));
    EXPECT_FALSE(
        dartplant::SnapshotSymbolMatchesContract(to_dynamic(base), headers, PF_R | PF_X, false));

    Elf64_Sym local = base;
    local.st_info = ELF64_ST_INFO(STB_LOCAL, STT_OBJECT);
    EXPECT_FALSE(dartplant::SnapshotSymbolMatchesContract(to_dynamic(local), headers, PF_R, false));
    EXPECT_FALSE(dartplant::SnapshotSymbolMatchesContract(to_section(local), headers, PF_R, false));

    Elf64_Sym function = base;
    function.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    EXPECT_FALSE(
        dartplant::SnapshotSymbolMatchesContract(to_dynamic(function), headers, PF_R, false));
    EXPECT_FALSE(
        dartplant::SnapshotSymbolMatchesContract(to_section(function), headers, PF_R, false));

    auto execute_only = headers;
    execute_only[0].flags = PF_X;
    EXPECT_FALSE(
        dartplant::SnapshotSymbolMatchesContract(to_section(base), execute_only, PF_R, false));
}

TEST_CASE(FlutterSnapshotSymbolContractRejectsMemszOnlyArtifactRanges) {
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R,
            .offset = 0x100,
            .virtual_address = 0x1000,
            .file_size = 0x100,
            .memory_size = 0x200,
        },
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0x300,
            .virtual_address = 0x2000,
            .file_size = 0x100,
            .memory_size = 0x200,
        },
    };
    const auto make_symbol = [](uint64_t value, uint64_t size) {
        return dartplant::ElfDynamicSymbol{
            .value = value,
            .size = size,
            .info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT),
            .section_index = 1,
        };
    };

    const auto data_tail = make_symbol(0x1180, 0x20);
    EXPECT_TRUE(dartplant::SnapshotSymbolMatchesContract(data_tail, headers, PF_R, true));
    EXPECT_FALSE(dartplant::SnapshotSymbolMatchesContract(data_tail, headers, PF_R, false));

    const auto instructions_tail = make_symbol(0x2180, 0x20);
    EXPECT_TRUE(
        dartplant::SnapshotSymbolMatchesContract(instructions_tail, headers, PF_R | PF_X, true));
    EXPECT_FALSE(
        dartplant::SnapshotSymbolMatchesContract(instructions_tail, headers, PF_R | PF_X, false));
}

TEST_CASE(FlutterSectionFallbackRejectsNonContractSymbolFacts) {
    auto bytes = MakeSectionSymbolElf();
    Elf64_Sym symbol{};
    constexpr size_t kSymbolOffset = 0x200 + sizeof(Elf64_Sym);
    std::memcpy(&symbol, bytes.data() + kSymbolOffset, sizeof(symbol));

    symbol.st_info = ELF64_ST_INFO(STB_LOCAL, STT_OBJECT);
    WriteAt(&bytes, kSymbolOffset, symbol);
    auto result = dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));

    symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    WriteAt(&bytes, kSymbolOffset, symbol);
    result = dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(FlutterSectionFallbackRequiresSymbolLinkToStringTable) {
    auto bytes = MakeSectionSymbolElf();
    Elf64_Shdr strings{};
    constexpr size_t kStringSectionOffset = 0x100 + 2 * sizeof(Elf64_Shdr);
    std::memcpy(&strings, bytes.data() + kStringSectionOffset, sizeof(strings));
    strings.sh_type = SHT_PROGBITS;
    WriteAt(&bytes, kStringSectionOffset, strings);

    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(FlutterSectionFallbackToleratesIdenticalDuplicateSymbols) {
    auto bytes = MakeSectionSymbolElf();
    Elf64_Shdr symbols{};
    constexpr size_t kSymbolSectionOffset = 0x100 + sizeof(Elf64_Shdr);
    std::memcpy(&symbols, bytes.data() + kSymbolSectionOffset, sizeof(symbols));
    symbols.sh_size = 3 * sizeof(Elf64_Sym);
    WriteAt(&bytes, kSymbolSectionOffset, symbols);
    Elf64_Sym duplicate{};
    std::memcpy(&duplicate, bytes.data() + 0x200 + sizeof(Elf64_Sym), sizeof(duplicate));
    WriteAt(&bytes, 0x200 + 2 * sizeof(Elf64_Sym), duplicate);

    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kFound),
              static_cast<uint32_t>(result.status));
    EXPECT_EQ(0x5000U, result.symbol.value);
}

TEST_CASE(FlutterSectionFallbackRejectsDistinctDuplicateSymbols) {
    auto bytes = MakeSectionSymbolElf();
    Elf64_Shdr symbols{};
    constexpr size_t kSymbolSectionOffset = 0x100 + sizeof(Elf64_Shdr);
    std::memcpy(&symbols, bytes.data() + kSymbolSectionOffset, sizeof(symbols));
    symbols.sh_size = 3 * sizeof(Elf64_Sym);
    WriteAt(&bytes, kSymbolSectionOffset, symbols);
    Elf64_Sym duplicate{};
    std::memcpy(&duplicate, bytes.data() + 0x200 + sizeof(Elf64_Sym), sizeof(duplicate));
    duplicate.st_value = 0x5010;
    WriteAt(&bytes, 0x200 + 2 * sizeof(Elf64_Sym), duplicate);

    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(FlutterSectionFallbackRejectsUnterminatedStringInsideShSize) {
    const auto bytes = MakeSectionSymbolElf(1, false);
    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(FlutterSectionFallbackRejectsStNameOutsideStringTable) {
    const auto bytes = MakeSectionSymbolElf(8, true);
    const auto result =
        dartplant::FindElfSectionSymbol(bytes, SectionSymbolProgramHeaders(), "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(FlutterDynamicSysvDiscoveryRunsEndToEndWithoutSectionTable) {
    const auto bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kSysv);
    Elf64_Ehdr header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    EXPECT_EQ(0U, header.e_shnum);

    TemporaryElfFile file(bytes);
    const auto module = MakeSyntheticModule(bytes, file.path());
    EXPECT_EQ(3U, module.executable_ranges.size());
    EXPECT_EQ(kSyntheticInstructionsVa, module.executable_ranges.back().virtual_address);

    std::string error;
    const auto snapshot = dartplant::DiscoverFlutterSnapshot(module, &error);
    EXPECT_TRUE(snapshot.has_value());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(std::string("flutter-arm64-product-compressed"), snapshot->profile_name);
    EXPECT_EQ(std::string("0123456789abcdef0123456789abcdef"), snapshot->snapshot_hash);
    EXPECT_EQ(kSyntheticInstructionsVa, snapshot->isolate_instructions_va);
    EXPECT_EQ(kSyntheticInstructionsSize, snapshot->isolate_instructions_size);
    EXPECT_EQ(kSyntheticLoadBias + kSyntheticInstructionsVa,
              snapshot->isolate_instructions_runtime);
    EXPECT_TRUE(snapshot->compressed_pointers);

    const auto resolved = snapshot->ResolveInstructionOffset(module, 0x20);
    EXPECT_TRUE(resolved.has_value());
    EXPECT_EQ(kSyntheticLoadBias + kSyntheticInstructionsVa + 0x20, *resolved);
}

TEST_CASE(FlutterDynamicGnuDiscoveryRunsEndToEndWithoutSectionTable) {
    const auto bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kGnu);
    TemporaryElfFile file(bytes);
    const auto module = MakeSyntheticModule(bytes, file.path());

    std::string error;
    const auto snapshot = dartplant::DiscoverFlutterSnapshot(module, &error);
    EXPECT_TRUE(snapshot.has_value());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(std::string("flutter-arm64-product-compressed"), snapshot->profile_name);
    EXPECT_EQ(kSyntheticInstructionsVa, snapshot->isolate_instructions_va);
    EXPECT_EQ(kSyntheticLoadBias + kSyntheticInstructionsVa,
              snapshot->isolate_instructions_runtime);
}

TEST_CASE(FlutterDeferredDynamicDiscoveryUsesIsolateSnapshotSymbols) {
    constexpr uint32_t kProgramHash = 0x1a30145f;
    const auto bytes =
        MakeDynamicSnapshotElf(DynamicHashStyle::kSysv, false, false, true, kProgramHash);
    TemporaryElfFile file(bytes);
    auto module = MakeSyntheticModule(bytes, file.path());
    module.name = "libapp.so-2.part.so";

    std::string error;
    const auto snapshot = dartplant::DiscoverDeferredFlutterSnapshot(module, &error);
    EXPECT_TRUE(snapshot.has_value());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(std::string("0123456789abcdef0123456789abcdef"), snapshot->snapshot_hash);
    EXPECT_EQ(kSyntheticInstructionsVa, snapshot->isolate_instructions_va);
    EXPECT_EQ(kSyntheticLoadBias + kSyntheticInstructionsVa,
              snapshot->isolate_instructions_runtime);
    EXPECT_EQ(kProgramHash, snapshot->deferred_program_hash.value_or(0));
}

TEST_CASE(FlutterDeferredDynamicDiscoveryRequiresSerializedProgramHash) {
    const auto bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kSysv);
    TemporaryElfFile file(bytes);
    auto module = MakeSyntheticModule(bytes, file.path());
    module.name = "libapp.so-2.part.so";

    std::string error;
    EXPECT_FALSE(dartplant::DiscoverDeferredFlutterSnapshot(module, &error).has_value());
    EXPECT_EQ(std::string("deferred Dart program hash is malformed"), error);
}

TEST_CASE(FlutterDynamicDiscoveryRequiresCanonicalProgramHeaderOffset) {
    auto bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kSysv);
    constexpr size_t kDynamicHeaderOffset = sizeof(Elf64_Ehdr) + 4 * sizeof(Elf64_Phdr);
    Elf64_Phdr dynamic{};
    std::memcpy(&dynamic, bytes.data() + kDynamicHeaderOffset, sizeof(dynamic));
    ++dynamic.p_offset;
    WriteAt(&bytes, kDynamicHeaderOffset, dynamic);

    TemporaryElfFile file(bytes);
    const auto module = MakeSyntheticModule(bytes, file.path());
    std::string error;
    EXPECT_FALSE(dartplant::DiscoverFlutterSnapshot(module, &error).has_value());
    EXPECT_EQ(std::string("Flutter app PT_DYNAMIC snapshot symbols are malformed"), error);
}

TEST_CASE(FlutterDynamicDiscoveryRequiresDtNullInsideFilesz) {
    auto bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kSysv);
    constexpr size_t kDynamicHeaderOffset = sizeof(Elf64_Ehdr) + 4 * sizeof(Elf64_Phdr);
    Elf64_Phdr dynamic{};
    std::memcpy(&dynamic, bytes.data() + kDynamicHeaderOffset, sizeof(dynamic));
    dynamic.p_filesz -= sizeof(Elf64_Dyn);
    WriteAt(&bytes, kDynamicHeaderOffset, dynamic);

    TemporaryElfFile file(bytes);
    const auto module = MakeSyntheticModule(bytes, file.path());
    std::string error;
    EXPECT_FALSE(dartplant::DiscoverFlutterSnapshot(module, &error).has_value());
    EXPECT_EQ(std::string("Flutter app PT_DYNAMIC snapshot symbols are malformed"), error);
}

TEST_CASE(FlutterSectionFallbackRunsEndToEndWhenPtDynamicIsMissing) {
    auto bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kSysv, true, true);
    // The exact same section table used by the malformed-dynamic test is a
    // valid compatibility source when PT_DYNAMIC is genuinely absent.
    for (size_t index : {size_t{4}, size_t{5}}) {
        Elf64_Phdr phdr{};
        const size_t offset = sizeof(Elf64_Ehdr) + index * sizeof(Elf64_Phdr);
        std::memcpy(&phdr, bytes.data() + offset, sizeof(phdr));
        phdr.p_type = PT_NULL;
        WriteAt(&bytes, offset, phdr);
    }

    TemporaryElfFile file(bytes);
    const auto module = MakeSyntheticModule(bytes, file.path());
    std::string error;
    const auto snapshot = dartplant::DiscoverFlutterSnapshot(module, &error);
    EXPECT_TRUE(snapshot.has_value());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(kSyntheticInstructionsVa, snapshot->isolate_instructions_va);
    EXPECT_EQ(kSyntheticLoadBias + kSyntheticInstructionsVa,
              snapshot->isolate_instructions_runtime);
}

TEST_CASE(FlutterMalformedPtDynamicCannotBeMaskedByValidSectionFallback) {
    const auto bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kSysv, true, true);
    std::vector<dartplant::ElfProgramHeaderView> headers;
    EXPECT_TRUE(dartplant::ParseElf64ProgramHeaders(bytes, &headers));

    // Prove the supplementary source is independently valid. The discovery
    // failure below must therefore come from the duplicated PT_DYNAMIC being
    // authoritative-malformed rather than from broken section fixtures.
    const auto data = dartplant::FindElfSectionSymbol(bytes, headers, "_kDartIsolateSnapshotData");
    const auto instructions =
        dartplant::FindElfSectionSymbol(bytes, headers, "_kDartIsolateSnapshotInstructions");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kFound),
              static_cast<uint32_t>(data.status));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfSectionLookupStatus::kFound),
              static_cast<uint32_t>(instructions.status));

    TemporaryElfFile file(bytes);
    const auto module = MakeSyntheticModule(bytes, file.path());
    std::string error;
    const auto snapshot = dartplant::DiscoverFlutterSnapshot(module, &error);
    EXPECT_FALSE(snapshot.has_value());
    EXPECT_EQ(std::string("Flutter app PT_DYNAMIC snapshot symbols are malformed"), error);
}

TEST_CASE(FlutterDynamicDiscoveryRejectsNonExecutableInstructionSegment) {
    const auto bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kSysv, false, false, false);
    TemporaryElfFile file(bytes);
    const auto module = MakeSyntheticModule(bytes, file.path());

    std::string error;
    const auto snapshot = dartplant::DiscoverFlutterSnapshot(module, &error);
    EXPECT_FALSE(snapshot.has_value());
    EXPECT_EQ(std::string("Flutter app PT_DYNAMIC snapshot symbols are malformed"), error);
}
