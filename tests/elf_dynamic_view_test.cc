// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <elf.h>

#include <array>
#include <cstring>
#include <vector>

#include "elf/dynamic_view.h"
#include "test_runner.h"

namespace {

template <typename T>
void WriteAt(std::vector<uint8_t>* bytes, size_t offset, const T& value) {
    EXPECT_TRUE(bytes != nullptr);
    EXPECT_TRUE(offset <= bytes->size());
    EXPECT_TRUE(sizeof(T) <= bytes->size() - offset);
    std::memcpy(bytes->data() + offset, &value, sizeof(T));
}

}  // namespace

TEST_CASE(ElfFileAndLoadedReadersUseDifferentPtLoadBounds) {
    std::vector<uint8_t> image(0x400, 0);
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_W,
            .offset = 0x40,
            .virtual_address = 0x1000,
            .file_size = 0x100,
            .memory_size = 0x180,
        },
    };
    image[0x13f] = 0x5a;

    dartplant::FileElfReader file_reader(image, headers);
    uint8_t value = 0;
    EXPECT_TRUE(file_reader.Read(0x10ff, &value, 1));
    EXPECT_EQ(0x5aU, value);
    EXPECT_FALSE(file_reader.Read(0x1100, &value, 1));
    EXPECT_FALSE(dartplant::ElfVaToFileOffset(headers, 0x1100).has_value());

    std::vector<uint8_t> mapped(0x180, 0);
    mapped[0x100] = 0xa5;
    const uintptr_t load_bias = reinterpret_cast<uintptr_t>(mapped.data()) - 0x1000;
    dartplant::LoadedElfReader loaded_reader(load_bias, headers);
    EXPECT_TRUE(loaded_reader.Read(0x1100, &value, 1));
    EXPECT_EQ(0xa5U, value);
    EXPECT_FALSE(loaded_reader.Read(0x1180, &value, 1));

    EXPECT_TRUE(dartplant::ElfVaRangeHasFlags(headers, 0x1100, 0x40, PF_R, true));
    EXPECT_FALSE(dartplant::ElfVaRangeHasFlags(headers, 0x1100, 1, PF_R, false));
}

TEST_CASE(ElfLoadedReaderRejectsExecuteOnlyLoadEvenWithinMemsz) {
    std::vector<uint8_t> mapped(0x80, 0x5a);
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_X,
            .offset = 0,
            .virtual_address = 0x3000,
            .file_size = 0x80,
            .memory_size = 0x80,
        },
    };
    const uintptr_t load_bias = reinterpret_cast<uintptr_t>(mapped.data()) - 0x3000;
    dartplant::LoadedElfReader reader(load_bias, headers);
    uint8_t value = 0;
    EXPECT_FALSE(reader.ReadableBytes(0x3000).has_value());
    EXPECT_FALSE(reader.Read(0x3000, &value, 1));
}

TEST_CASE(ElfVaToFileOffsetRequiresWholeRangeInsideFilesz) {
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R,
            .offset = 0x800,
            .virtual_address = 0x2000,
            .file_size = 0x100,
            .memory_size = 0x300,
        },
    };
    const auto first = dartplant::ElfVaToFileOffset(headers, 0x2000, 4);
    EXPECT_TRUE(first.has_value());
    EXPECT_EQ(0x800U, *first);
    const auto last = dartplant::ElfVaToFileOffset(headers, 0x20fc, 4);
    EXPECT_TRUE(last.has_value());
    EXPECT_EQ(0x8fcU, *last);
    EXPECT_FALSE(dartplant::ElfVaToFileOffset(headers, 0x20fd, 4).has_value());
    EXPECT_FALSE(dartplant::ElfVaToFileOffset(headers, 0x2100, 1).has_value());
}

TEST_CASE(ElfDynamicViewKeepsDynamicPointersAsElfVirtualAddresses) {
    std::vector<uint8_t> image(0x500, 0);
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_W,
            .offset = 0x100,
            .virtual_address = 0x2000,
            .file_size = 0x300,
            .memory_size = 0x300,
        },
    };
    const std::array dynamic = {
        Elf64_Dyn{.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x2080}},
        Elf64_Dyn{.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x20c0}},
        Elf64_Dyn{.d_tag = DT_STRSZ, .d_un = {.d_val = 0x20}},
        Elf64_Dyn{.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
        Elf64_Dyn{.d_tag = DT_HASH, .d_un = {.d_ptr = 0x20e0}},
        Elf64_Dyn{.d_tag = DT_NULL, .d_un = {.d_val = 0}},
    };
    std::memcpy(image.data() + 0x100, dynamic.data(), sizeof(dynamic));
    dartplant::FileElfReader reader(image, headers);
    const auto result = dartplant::ReadElfDynamicView(reader, 0x2000, sizeof(dynamic));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kAvailable),
              static_cast<uint32_t>(result.status));
    EXPECT_EQ(0x2080U, result.view.symtab_va);
    EXPECT_EQ(0x20c0U, result.view.strtab_va);
    EXPECT_EQ(0x20U, result.view.strtab_size);
    EXPECT_TRUE(result.view.sysv_hash_va.has_value());
    EXPECT_EQ(0x20e0U, *result.view.sysv_hash_va);
    EXPECT_FALSE(result.view.gnu_hash_va.has_value());
}

TEST_CASE(ElfDynamicViewRecordsGnuHashWithoutInterpretingItsLayout) {
    std::vector<uint8_t> image(0x500, 0);
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R,
            .offset = 0x80,
            .virtual_address = 0x3000,
            .file_size = 0x300,
            .memory_size = 0x300,
        },
    };
    const std::array dynamic = {
        Elf64_Dyn{.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x3080}},
        Elf64_Dyn{.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x30c0}},
        Elf64_Dyn{.d_tag = DT_STRSZ, .d_un = {.d_val = 0x20}},
        Elf64_Dyn{.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
        Elf64_Dyn{.d_tag = DT_GNU_HASH, .d_un = {.d_ptr = 0x3100}},
        Elf64_Dyn{.d_tag = DT_NULL, .d_un = {.d_val = 0}},
    };
    std::memcpy(image.data() + 0x80, dynamic.data(), sizeof(dynamic));
    dartplant::FileElfReader reader(image, headers);
    const auto result = dartplant::ReadElfDynamicView(reader, 0x3000, sizeof(dynamic));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kAvailable),
              static_cast<uint32_t>(result.status));
    EXPECT_TRUE(result.view.gnu_hash_va.has_value());
    EXPECT_EQ(0x3100U, *result.view.gnu_hash_va);
    EXPECT_FALSE(result.view.sysv_hash_va.has_value());
}

TEST_CASE(ElfDynamicViewDoesNotRequireAHashIndex) {
    std::vector<uint8_t> image(0x300, 0);
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R,
            .offset = 0x40,
            .virtual_address = 0x6000,
            .file_size = 0x200,
            .memory_size = 0x200,
        },
    };
    const std::array dynamic = {
        Elf64_Dyn{.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x6080}},
        Elf64_Dyn{.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x60c0}},
        Elf64_Dyn{.d_tag = DT_STRSZ, .d_un = {.d_val = 0x20}},
        Elf64_Dyn{.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
        Elf64_Dyn{.d_tag = DT_NULL, .d_un = {.d_val = 0}},
    };
    std::memcpy(image.data() + 0x40, dynamic.data(), sizeof(dynamic));
    dartplant::FileElfReader reader(image, headers);
    const auto result = dartplant::ReadElfDynamicView(reader, 0x6000, sizeof(dynamic));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kAvailable),
              static_cast<uint32_t>(result.status));
    EXPECT_FALSE(result.view.sysv_hash_va.has_value());
    EXPECT_FALSE(result.view.gnu_hash_va.has_value());
    const auto lookup = dartplant::FindElfDynamicSymbol(reader, result.view, "symbol");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kUnavailable),
              static_cast<uint32_t>(lookup.status));
}

TEST_CASE(ElfDynamicViewRejectsUnterminatedOrOutOfBoundsTables) {
    std::vector<uint8_t> image(0x400, 0);
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_W,
            .offset = 0x40,
            .virtual_address = 0x4000,
            .file_size = 0x180,
            .memory_size = 0x200,
        },
    };
    const std::array unterminated = {
        Elf64_Dyn{.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x4080}},
        Elf64_Dyn{.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x40c0}},
    };
    std::memcpy(image.data() + 0x40, unterminated.data(), sizeof(unterminated));
    dartplant::FileElfReader reader(image, headers);
    auto result = dartplant::ReadElfDynamicView(reader, 0x4000, sizeof(unterminated));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kMalformed),
              static_cast<uint32_t>(result.status));

    const std::array bad_strtab = {
        Elf64_Dyn{.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x4080}},
        Elf64_Dyn{.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x4170}},
        Elf64_Dyn{.d_tag = DT_STRSZ, .d_un = {.d_val = 0x20}},
        Elf64_Dyn{.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
        Elf64_Dyn{.d_tag = DT_HASH, .d_un = {.d_ptr = 0x4100}},
        Elf64_Dyn{.d_tag = DT_NULL, .d_un = {.d_val = 0}},
    };
    std::memcpy(image.data() + 0x40, bad_strtab.data(), sizeof(bad_strtab));
    result = dartplant::ReadElfDynamicView(reader, 0x4000, sizeof(bad_strtab));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfDynamicViewRejectsWrongSymbolEntrySize) {
    std::vector<uint8_t> image(0x300, 0);
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R,
            .offset = 0x20,
            .virtual_address = 0x5000,
            .file_size = 0x200,
            .memory_size = 0x200,
        },
    };
    const std::array dynamic = {
        Elf64_Dyn{.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x5080}},
        Elf64_Dyn{.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x50c0}},
        Elf64_Dyn{.d_tag = DT_STRSZ, .d_un = {.d_val = 0x20}},
        Elf64_Dyn{.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym) + 8}},
        Elf64_Dyn{.d_tag = DT_HASH, .d_un = {.d_ptr = 0x5100}},
        Elf64_Dyn{.d_tag = DT_NULL, .d_un = {.d_val = 0}},
    };
    std::memcpy(image.data() + 0x20, dynamic.data(), sizeof(dynamic));
    dartplant::FileElfReader reader(image, headers);
    const auto result = dartplant::ReadElfDynamicView(reader, 0x5000, sizeof(dynamic));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfDynamicViewRejectsPartialDynamicEntryTail) {
    std::vector<uint8_t> image(0x200, 0);
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R,
            .offset = 0x20,
            .virtual_address = 0x7000,
            .file_size = 0x100,
            .memory_size = 0x100,
        },
    };
    Elf64_Dyn null_entry{.d_tag = DT_NULL, .d_un = {.d_val = 0}};
    WriteAt(&image, 0x20, null_entry);
    dartplant::FileElfReader reader(image, headers);
    const auto result = dartplant::ReadElfDynamicView(reader, 0x7000, sizeof(Elf64_Dyn) + 1);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfProgramHeaderParserDoesNotNeedSectionHeaders) {
    std::vector<uint8_t> bytes(sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr), 0);
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
    header.e_phnum = 2;
    header.e_shoff = 0;
    header.e_shnum = 0;
    WriteAt(&bytes, 0, header);

    Elf64_Phdr load{};
    load.p_type = PT_LOAD;
    load.p_flags = PF_R | PF_X;
    load.p_offset = 0;
    load.p_vaddr = 0;
    load.p_filesz = bytes.size();
    load.p_memsz = bytes.size();
    WriteAt(&bytes, sizeof(Elf64_Ehdr), load);

    Elf64_Phdr dynamic{};
    dynamic.p_type = PT_DYNAMIC;
    dynamic.p_flags = PF_R;
    dynamic.p_offset = 0x40;
    dynamic.p_vaddr = 0x40;
    dynamic.p_filesz = sizeof(Elf64_Dyn) * 2;
    dynamic.p_memsz = dynamic.p_filesz;
    WriteAt(&bytes, sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr), dynamic);

    std::vector<dartplant::ElfProgramHeaderView> parsed;
    EXPECT_TRUE(dartplant::ParseElf64ProgramHeaders(bytes, &parsed));
    EXPECT_EQ(2U, parsed.size());
    const auto parsed_dynamic = dartplant::FindElfProgramHeader(parsed, PT_DYNAMIC);
    EXPECT_TRUE(parsed_dynamic.has_value());
    EXPECT_EQ(0x40U, parsed_dynamic->virtual_address);
}

TEST_CASE(ElfProgramHeaderParserRejectsWrongMachineOrImpossibleSegmentSize) {
    std::vector<uint8_t> bytes(sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr), 0);
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
    header.e_phnum = 1;
    WriteAt(&bytes, 0, header);

    Elf64_Phdr phdr{};
    phdr.p_type = PT_LOAD;
    phdr.p_filesz = 0x200;
    phdr.p_memsz = 0x100;
    WriteAt(&bytes, sizeof(Elf64_Ehdr), phdr);
    std::vector<dartplant::ElfProgramHeaderView> parsed;
    EXPECT_FALSE(dartplant::ParseElf64ProgramHeaders(bytes, &parsed));

    phdr.p_filesz = phdr.p_memsz;
    WriteAt(&bytes, sizeof(Elf64_Ehdr), phdr);
    header.e_machine = EM_X86_64;
    WriteAt(&bytes, 0, header);
    EXPECT_FALSE(dartplant::ParseElf64ProgramHeaders(bytes, &parsed));
}
