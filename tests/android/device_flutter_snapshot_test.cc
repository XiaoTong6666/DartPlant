// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <elf.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/internal.h"
#include "elf/dynamic_view.h"
#include "runtime/flutter_snapshot_internal.h"

namespace {

enum class DynamicHashStyle {
    kSysv,
    kGnu,
};

constexpr uint64_t kSyntheticDataVa = 0x1000;
constexpr uint64_t kSyntheticInstructionsVa = 0x7000;
constexpr uint64_t kSyntheticInstructionsSize = 0x100;
constexpr uintptr_t kSyntheticLoadBias = static_cast<uintptr_t>(0x123450000000ULL);

template <typename T>
void WriteAt(std::vector<uint8_t>* bytes, size_t offset, const T& value) {
    std::memcpy(bytes->data() + offset, &value, sizeof(T));
}

std::vector<uint8_t> MakeFullAotSnapshotHeader() {
    constexpr size_t kHeaderSize = 20;
    constexpr char kHash[] = "0123456789abcdef0123456789abcdef";
    constexpr std::string_view kFeatures = "product arm64 android compressed-pointers null-safety";
    const size_t total = kHeaderSize + 32 + kFeatures.size() + 1;
    std::vector<uint8_t> bytes(total, 0);
    const int32_t magic = static_cast<int32_t>(0xdcdcf5f5U);
    const int64_t stored_length = static_cast<int64_t>(total - sizeof(int32_t));
    const int64_t kind = 3;
    std::memcpy(bytes.data(), &magic, sizeof(magic));
    std::memcpy(bytes.data() + 4, &stored_length, sizeof(stored_length));
    std::memcpy(bytes.data() + 12, &kind, sizeof(kind));
    std::memcpy(bytes.data() + kHeaderSize, kHash, 32);
    std::memcpy(bytes.data() + kHeaderSize + 32, kFeatures.data(), kFeatures.size());
    return bytes;
}

uint32_t GnuHash(std::string_view name) {
    uint32_t hash = 5381;
    for (const unsigned char value : name) hash = hash * 33U + value;
    return hash;
}

std::vector<uint8_t> MakeDynamicSnapshotElf(DynamicHashStyle hash_style,
                                            bool duplicate_dynamic = false,
                                            bool add_valid_sections = false,
                                            bool executable_instructions = true) {
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

    const auto snapshot = MakeFullAotSnapshotHeader();
    std::vector<uint8_t> bytes(0x4000, 0);

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
    header.e_phnum = 5 + (duplicate_dynamic ? 1 : 0);
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
    metadata.p_filesz = 0x1800;
    metadata.p_memsz = 0x1800;
    write_phdr(0, metadata);

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
    std::memcpy(bytes.data() + kStrtabOffset + data_name_offset, kDataName.data(),
                kDataName.size());
    std::memcpy(bytes.data() + kStrtabOffset + instructions_name_offset, kInstructionsName.data(),
                kInstructionsName.size());

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
        const std::array<uint32_t, 6> hash = {1, 3, 1, 0, 2, 0};
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

class TemporaryElfFile final {
public:
    explicit TemporaryElfFile(const std::vector<uint8_t>& bytes) {
        std::array<char, 64> pattern{};
        const int written = std::snprintf(pattern.data(), pattern.size(),
                                          "./dartplant-snapshot-%d-XXXXXX", getpid());
        if (written <= 0 || static_cast<size_t>(written) >= pattern.size()) return;
        const int fd = mkstemp(pattern.data());
        if (fd < 0) return;
        path_ = pattern.data();
        size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = write(fd, bytes.data() + offset, bytes.size() - offset);
            if (count <= 0) {
                path_.clear();
                break;
            }
            offset += static_cast<size_t>(count);
        }
        close(fd);
        if (path_.empty()) std::remove(pattern.data());
    }

    ~TemporaryElfFile() {
        if (!path_.empty()) std::remove(path_.c_str());
    }

    bool valid() const { return !path_.empty(); }
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

bool MakeSyntheticModule(const std::vector<uint8_t>& bytes, const std::string& path,
                         dartplant::ModuleImage* module) {
    std::vector<dartplant::ElfProgramHeaderView> headers;
    if (!dartplant::ParseElf64ProgramHeaders(bytes, &headers)) return false;
    if (!dartplant::BuildModuleImageFromProgramHeaders(path, kSyntheticLoadBias, headers, module)) {
        return false;
    }
    module->build_id = "synthetic-build-id";
    return true;
}

int FailDiscovery(const char* name, std::string_view detail) {
    std::fprintf(stderr, "[FAIL] %s: %.*s\n", name, static_cast<int>(detail.size()), detail.data());
    return 1;
}

int RunSuccessfulDiscovery(const char* name, DynamicHashStyle style) {
    const auto bytes = MakeDynamicSnapshotElf(style);
    TemporaryElfFile file(bytes);
    if (!file.valid()) return FailDiscovery(name, "cannot create device synthetic ELF");
    dartplant::ModuleImage module;
    if (!MakeSyntheticModule(bytes, file.path(), &module)) {
        return FailDiscovery(name, "cannot construct synthetic ModuleImage");
    }
    std::string error;
    const auto snapshot = dartplant::DiscoverFlutterSnapshot(module, &error);
    if (!snapshot.has_value() || !error.empty() ||
        snapshot->profile_name != "flutter-arm64-product-compressed" ||
        snapshot->isolate_instructions_va != kSyntheticInstructionsVa ||
        snapshot->isolate_instructions_runtime != kSyntheticLoadBias + kSyntheticInstructionsVa ||
        module.executable_ranges.size() != 3 ||
        module.executable_ranges.back().virtual_address != kSyntheticInstructionsVa) {
        return FailDiscovery(name, error.empty() ? "unexpected discovery result" : error);
    }
    std::fprintf(stdout, "[PASS] %s\n", name);
    return 0;
}

}  // namespace

int RunDeviceFlutterSnapshotDiscoveryTests() {
    if (RunSuccessfulDiscovery("ARM64 Flutter SysV PT_DYNAMIC discovery",
                               DynamicHashStyle::kSysv) != 0) {
        return 1;
    }
    if (RunSuccessfulDiscovery("ARM64 Flutter GNU PT_DYNAMIC discovery", DynamicHashStyle::kGnu) !=
        0) {
        return 1;
    }

    auto fallback_bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kSysv, true, true);
    for (size_t index : {size_t{4}, size_t{5}}) {
        const size_t offset = sizeof(Elf64_Ehdr) + index * sizeof(Elf64_Phdr);
        Elf64_Phdr phdr{};
        std::memcpy(&phdr, fallback_bytes.data() + offset, sizeof(phdr));
        phdr.p_type = PT_NULL;
        WriteAt(&fallback_bytes, offset, phdr);
    }
    {
        TemporaryElfFile file(fallback_bytes);
        dartplant::ModuleImage module;
        if (!file.valid() || !MakeSyntheticModule(fallback_bytes, file.path(), &module)) {
            return FailDiscovery("ARM64 Flutter section fallback discovery",
                                 "fixture setup failed");
        }
        std::string error;
        const auto snapshot = dartplant::DiscoverFlutterSnapshot(module, &error);
        if (!snapshot.has_value() || !error.empty() ||
            snapshot->isolate_instructions_runtime !=
                kSyntheticLoadBias + kSyntheticInstructionsVa) {
            return FailDiscovery("ARM64 Flutter section fallback discovery",
                                 error.empty() ? "unexpected discovery result" : error);
        }
        std::fprintf(stdout, "[PASS] ARM64 Flutter section fallback discovery\n");
    }

    {
        const auto bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kSysv, true, true);
        std::vector<dartplant::ElfProgramHeaderView> headers;
        if (!dartplant::ParseElf64ProgramHeaders(bytes, &headers)) {
            return FailDiscovery("ARM64 duplicate PT_DYNAMIC fail-closed",
                                 "fixture program headers were not valid");
        }
        const auto data =
            dartplant::FindElfSectionSymbol(bytes, headers, "_kDartIsolateSnapshotData");
        const auto instructions =
            dartplant::FindElfSectionSymbol(bytes, headers, "_kDartIsolateSnapshotInstructions");
        if (data.status != dartplant::ElfSectionLookupStatus::kFound ||
            instructions.status != dartplant::ElfSectionLookupStatus::kFound) {
            return FailDiscovery("ARM64 duplicate PT_DYNAMIC fail-closed",
                                 "valid section fallback fixture was not valid");
        }
        TemporaryElfFile file(bytes);
        dartplant::ModuleImage module;
        if (!file.valid() || !MakeSyntheticModule(bytes, file.path(), &module)) {
            return FailDiscovery("ARM64 duplicate PT_DYNAMIC fail-closed", "fixture setup failed");
        }
        std::string error;
        const auto snapshot = dartplant::DiscoverFlutterSnapshot(module, &error);
        if (snapshot.has_value() ||
            error != "Flutter app PT_DYNAMIC snapshot symbols are malformed") {
            return FailDiscovery("ARM64 duplicate PT_DYNAMIC fail-closed", error);
        }
        std::fprintf(stdout, "[PASS] ARM64 duplicate PT_DYNAMIC fail-closed\n");
    }

    {
        const auto bytes = MakeDynamicSnapshotElf(DynamicHashStyle::kSysv, false, false, false);
        TemporaryElfFile file(bytes);
        dartplant::ModuleImage module;
        if (!file.valid() || !MakeSyntheticModule(bytes, file.path(), &module)) {
            return FailDiscovery("ARM64 non-executable instructions fail-closed",
                                 "fixture setup failed");
        }
        std::string error;
        const auto snapshot = dartplant::DiscoverFlutterSnapshot(module, &error);
        if (snapshot.has_value() ||
            error != "Flutter app PT_DYNAMIC snapshot symbols are malformed") {
            return FailDiscovery("ARM64 non-executable instructions fail-closed", error);
        }
        std::fprintf(stdout, "[PASS] ARM64 non-executable instructions fail-closed\n");
    }

    return 0;
}
