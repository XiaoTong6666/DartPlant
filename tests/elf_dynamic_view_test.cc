// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <elf.h>

#include <array>
#include <cstring>
#include <map>
#include <string_view>
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

uint32_t TestGnuHash(std::string_view name) {
    uint32_t hash = 5381;
    for (const unsigned char value : name) hash = hash * 33U + value;
    return hash;
}

struct DynamicLookupFixture {
    std::vector<uint8_t> image = std::vector<uint8_t>(0x700, 0);
    std::array<dartplant::ElfProgramHeaderView, 1> headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_W,
            .offset = 0,
            .virtual_address = 0x1000,
            .file_size = 0x700,
            .memory_size = 0x700,
        },
    };
    dartplant::ElfDynamicView view{
        .symtab_va = 0x1100,
        .strtab_va = 0x1200,
        .strtab_size = 8,
        .symbol_entry_size = sizeof(Elf64_Sym),
        .sysv_hash_va = std::nullopt,
        .gnu_hash_va = std::nullopt,
    };

    void WriteTargetSymbol(uint32_t name_offset = 1) {
        constexpr char kNames[] = "\0target\0";
        std::memcpy(image.data() + 0x200, kNames, sizeof(kNames) - 1);
        Elf64_Sym symbol{};
        symbol.st_name = name_offset;
        symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
        symbol.st_shndx = 1;
        symbol.st_value = 0x1600;
        symbol.st_size = 4;
        WriteAt(&image, 0x100 + sizeof(Elf64_Sym), symbol);
    }

    void WriteSysvHash() {
        view.sysv_hash_va = 0x1300;
        const std::array<uint32_t, 5> table = {1, 2, 1, 0, 0};
        std::memcpy(image.data() + 0x300, table.data(), sizeof(table));
    }

    void WriteGnuHash(uint32_t bucket_symbol = 1, bool terminate_chain = true,
                      uint32_t bloom_shift = 5) {
        view.gnu_hash_va = 0x1400;
        const uint32_t hash = TestGnuHash("target");
        const std::array<uint32_t, 4> header = {1, 1, 1, bloom_shift};
        std::memcpy(image.data() + 0x400, header.data(), sizeof(header));
        Elf64_Addr bloom = 0;
        if (bloom_shift < 32) {
            bloom =
                (Elf64_Addr{1} << (hash % 64)) | (Elf64_Addr{1} << ((hash >> bloom_shift) % 64));
        }
        WriteAt(&image, 0x410, bloom);
        WriteAt(&image, 0x418, bucket_symbol);
        const uint32_t chain = terminate_chain ? (hash | 1U) : (hash & ~1U);
        WriteAt(&image, 0x41c, chain);
    }
};

class SparseElfReader final : public dartplant::ElfVaReader {
public:
    bool Read(uint64_t va, void* output, size_t size) const override {
        if (output == nullptr || (size != 0 && va > UINT64_MAX - (size - 1))) return false;
        auto* destination = static_cast<uint8_t*>(output);
        for (size_t index = 0; index < size; ++index) {
            const auto found = bytes_.find(va + index);
            if (found == bytes_.end()) return false;
            destination[index] = found->second;
        }
        return true;
    }

    std::optional<uint64_t> ReadableBytes(uint64_t) const override { return 0x1000; }

    template <typename T>
    void Write(uint64_t va, const T& value) {
        const auto* source = reinterpret_cast<const uint8_t*>(&value);
        for (size_t index = 0; index < sizeof(T); ++index) {
            EXPECT_TRUE(va <= UINT64_MAX - index);
            bytes_[va + index] = source[index];
        }
    }

private:
    std::map<uint64_t, uint8_t> bytes_;
};

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

    dartplant::FileBackedLoadedElfReader artifact_reader(load_bias, headers);
    EXPECT_TRUE(artifact_reader.Read(0x10ff, &value, 1));
    EXPECT_FALSE(artifact_reader.Read(0x1100, &value, 1));

    EXPECT_TRUE(dartplant::ElfVaRangeHasFlags(headers, 0x1100, 0x40, PF_R, true));
    EXPECT_FALSE(dartplant::ElfVaRangeHasFlags(headers, 0x1100, 1, PF_R, false));
}

TEST_CASE(ElfProgramHeaderCanonicalFileBackingRequiresExactReadableMapping) {
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R,
            .offset = 0x400,
            .virtual_address = 0x1000,
            .file_size = 0x100,
            .memory_size = 0x180,
        },
        dartplant::ElfProgramHeaderView{
            .type = PT_DYNAMIC,
            .flags = PF_R,
            .offset = 0x480,
            .virtual_address = 0x1080,
            .file_size = 0x20,
            .memory_size = 0x40,
        },
    };
    EXPECT_TRUE(dartplant::ElfProgramHeaderHasCanonicalFileBacking(headers, headers[1], PF_R));

    auto dynamic = headers[1];
    dynamic.offset = 0x481;
    EXPECT_FALSE(dartplant::ElfProgramHeaderHasCanonicalFileBacking(headers, dynamic, PF_R));
    dynamic = headers[1];
    dynamic.file_size = 0;
    EXPECT_FALSE(dartplant::ElfProgramHeaderHasCanonicalFileBacking(headers, dynamic, PF_R));
    dynamic = headers[1];
    dynamic.virtual_address = 0x1100;
    dynamic.offset = 0x500;
    EXPECT_FALSE(dartplant::ElfProgramHeaderHasCanonicalFileBacking(headers, dynamic, PF_R));

    auto execute_only_headers = headers;
    execute_only_headers[0].flags = PF_X;
    EXPECT_FALSE(dartplant::ElfProgramHeaderHasCanonicalFileBacking(execute_only_headers,
                                                                    execute_only_headers[1], PF_R));
}

TEST_CASE(ElfLoadedDynamicMetadataCannotUseMemszOnlyBytes) {
    std::vector<uint8_t> mapped(0x200, 0);
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R,
            .offset = 0,
            .virtual_address = 0x1000,
            .file_size = 0x100,
            .memory_size = 0x200,
        },
    };
    const std::array dynamic = {
        Elf64_Dyn{.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x1060}},
        Elf64_Dyn{.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x1080}},
        Elf64_Dyn{.d_tag = DT_STRSZ, .d_un = {.d_val = 8}},
        Elf64_Dyn{.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
        Elf64_Dyn{.d_tag = DT_HASH, .d_un = {.d_ptr = 0x1120}},
        Elf64_Dyn{.d_tag = DT_NULL, .d_un = {.d_val = 0}},
    };
    std::memcpy(mapped.data(), dynamic.data(), sizeof(dynamic));
    const std::array<uint32_t, 2> hash_header = {1, 1};
    std::memcpy(mapped.data() + 0x120, hash_header.data(), sizeof(hash_header));
    const uintptr_t load_bias = reinterpret_cast<uintptr_t>(mapped.data()) - 0x1000;

    dartplant::LoadedElfReader memory_reader(load_bias, headers);
    const auto memory_view = dartplant::ReadElfDynamicView(memory_reader, 0x1000, sizeof(dynamic));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kAvailable),
              static_cast<uint32_t>(memory_view.status));

    dartplant::FileBackedLoadedElfReader artifact_reader(load_bias, headers);
    const auto artifact_view =
        dartplant::ReadElfDynamicView(artifact_reader, 0x1000, sizeof(dynamic));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kMalformed),
              static_cast<uint32_t>(artifact_view.status));
}

TEST_CASE(ElfLoadedDynamicTableCannotFindDtNullInMemszTail) {
    const std::array dynamic_without_null = {
        Elf64_Dyn{.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x1000}},
        Elf64_Dyn{.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x1010}},
        Elf64_Dyn{.d_tag = DT_STRSZ, .d_un = {.d_val = 1}},
        Elf64_Dyn{.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
        Elf64_Dyn{.d_tag = DT_HASH, .d_un = {.d_ptr = 0x1020}},
    };
    constexpr size_t kMemorySize = sizeof(dynamic_without_null) + sizeof(Elf64_Dyn);
    std::vector<uint8_t> mapped(kMemorySize, 0);
    std::memcpy(mapped.data(), dynamic_without_null.data(), sizeof(dynamic_without_null));
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R,
            .offset = 0,
            .virtual_address = 0x1000,
            .file_size = sizeof(dynamic_without_null),
            .memory_size = kMemorySize,
        },
    };
    const uintptr_t load_bias = reinterpret_cast<uintptr_t>(mapped.data()) - 0x1000;
    dartplant::LoadedElfReader memory_reader(load_bias, headers);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kAvailable),
              static_cast<uint32_t>(
                  dartplant::ReadElfDynamicView(memory_reader, 0x1000, kMemorySize).status));

    dartplant::FileBackedLoadedElfReader artifact_reader(load_bias, headers);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kMalformed),
              static_cast<uint32_t>(
                  dartplant::ReadElfDynamicView(artifact_reader, 0x1000, kMemorySize).status));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kMalformed),
              static_cast<uint32_t>(dartplant::ReadElfDynamicView(artifact_reader, 0x1000,
                                                                  sizeof(dynamic_without_null))
                                        .status));
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

TEST_CASE(ElfVaToFileOffsetRejectsDistinctOverlappingLoadMappings) {
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .offset = 0x800,
            .virtual_address = 0x2000,
            .file_size = 0x100,
            .memory_size = 0x100,
        },
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .offset = 0x900,
            .virtual_address = 0x2000,
            .file_size = 0x100,
            .memory_size = 0x100,
        },
    };
    EXPECT_FALSE(dartplant::ElfVaToFileOffset(headers, 0x2000, 4).has_value());

    const std::array identical = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .offset = 0x800,
            .virtual_address = 0x2000,
            .file_size = 0x100,
            .memory_size = 0x100,
        },
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .offset = 0x800,
            .virtual_address = 0x2000,
            .file_size = 0x100,
            .memory_size = 0x100,
        },
    };
    const auto resolved = dartplant::ElfVaToFileOffset(identical, 0x2000, 4);
    EXPECT_TRUE(resolved.has_value());
    EXPECT_EQ(0x800U, *resolved);
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

TEST_CASE(ElfDynamicViewToleratesIdenticalSingletonsAndRejectsConflicts) {
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
    const auto read = [&](const std::vector<Elf64_Dyn>& entries) {
        std::memset(image.data() + 0x100, 0, 0x300);
        std::memcpy(image.data() + 0x100, entries.data(), entries.size() * sizeof(Elf64_Dyn));
        dartplant::FileElfReader reader(image, headers);
        return dartplant::ReadElfDynamicView(reader, 0x2000, entries.size() * sizeof(Elf64_Dyn));
    };
    const Elf64_Dyn null_entry{.d_tag = DT_NULL, .d_un = {.d_val = 0}};
    const std::vector<Elf64_Dyn> identical = {
        {.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x2080}},
        {.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x2080}},
        {.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x20c0}},
        {.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x20c0}},
        {.d_tag = DT_STRSZ, .d_un = {.d_val = 0x20}},
        {.d_tag = DT_STRSZ, .d_un = {.d_val = 0x20}},
        {.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
        {.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
        {.d_tag = DT_HASH, .d_un = {.d_ptr = 0x20e0}},
        {.d_tag = DT_HASH, .d_un = {.d_ptr = 0x20e0}},
        {.d_tag = DT_GNU_HASH, .d_un = {.d_ptr = 0x2100}},
        {.d_tag = DT_GNU_HASH, .d_un = {.d_ptr = 0x2100}},
        null_entry,
    };
    const auto available = read(identical);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kAvailable),
              static_cast<uint32_t>(available.status));

    const auto expect_conflict = [&](Elf64_Dyn first, Elf64_Dyn second) {
        std::vector<Elf64_Dyn> entries = {
            {.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x2080}},
            {.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x20c0}},
            {.d_tag = DT_STRSZ, .d_un = {.d_val = 0x20}},
            {.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
            {.d_tag = DT_HASH, .d_un = {.d_ptr = 0x20e0}},
        };
        entries.push_back(first);
        entries.push_back(second);
        entries.push_back(null_entry);
        const auto result = read(entries);
        EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kMalformed),
                  static_cast<uint32_t>(result.status));
    };
    expect_conflict({.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x2080}},
                    {.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x2090}});
    expect_conflict({.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x20c0}},
                    {.d_tag = DT_STRTAB, .d_un = {.d_ptr = 0x20d0}});
    expect_conflict({.d_tag = DT_STRSZ, .d_un = {.d_val = 0x20}},
                    {.d_tag = DT_STRSZ, .d_un = {.d_val = 0x21}});
    expect_conflict({.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
                    {.d_tag = DT_SYMENT, .d_un = {.d_val = 16}});
    expect_conflict({.d_tag = DT_HASH, .d_un = {.d_ptr = 0x20e0}},
                    {.d_tag = DT_HASH, .d_un = {.d_ptr = 0x20f0}});
    expect_conflict({.d_tag = DT_GNU_HASH, .d_un = {.d_ptr = 0x2100}},
                    {.d_tag = DT_GNU_HASH, .d_un = {.d_ptr = 0x2110}});
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

TEST_CASE(ElfDynamicLookupRejectsAuthoritativeDynamicTableWithoutHashIndex) {
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
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
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

TEST_CASE(ElfDynamicViewRejectsIncompleteAuthoritativeDynamicTable) {
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
    const std::array missing_strtab = {
        Elf64_Dyn{.d_tag = DT_SYMTAB, .d_un = {.d_ptr = 0x5080}},
        Elf64_Dyn{.d_tag = DT_STRSZ, .d_un = {.d_val = 0x20}},
        Elf64_Dyn{.d_tag = DT_SYMENT, .d_un = {.d_val = sizeof(Elf64_Sym)}},
        Elf64_Dyn{.d_tag = DT_NULL, .d_un = {.d_val = 0}},
    };
    std::memcpy(image.data() + 0x20, missing_strtab.data(), sizeof(missing_strtab));
    dartplant::FileElfReader reader(image, headers);

    auto result = dartplant::ReadElfDynamicView(reader, 0x5000, sizeof(missing_strtab));
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicViewStatus::kMalformed),
              static_cast<uint32_t>(result.status));

    result = dartplant::ReadElfDynamicView(reader, 0, sizeof(Elf64_Dyn));
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

TEST_CASE(ElfDynamicLookupFindsSysvHashOnlySymbol) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    fixture.WriteSysvHash();
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kFound),
              static_cast<uint32_t>(result.status));
    EXPECT_EQ(0x1600U, result.symbol.value);
}

TEST_CASE(ElfDynamicLookupFindsGnuHashOnlySymbol) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    fixture.WriteGnuHash();
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kFound),
              static_cast<uint32_t>(result.status));
    EXPECT_EQ(0x1600U, result.symbol.value);
}

TEST_CASE(ElfGnuHashAcceptsLldBloomShift) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    fixture.WriteGnuHash(1, true, 26);
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kFound),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfGnuHashAcceptsMaximumValidBloomShift) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    fixture.WriteGnuHash(1, true, 31);
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kFound),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfGnuHashRejectsBloomShiftEqualToHashWidth) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    fixture.WriteGnuHash(1, true, 32);
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfGnuHashRejectsMaximumEncodedBloomShift) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    fixture.WriteGnuHash(1, true, UINT32_MAX);
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfSysvHashLookupToleratesIdenticalDuplicateSymbols) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    Elf64_Sym duplicate{};
    std::memcpy(&duplicate, fixture.image.data() + 0x100 + sizeof(Elf64_Sym), sizeof(duplicate));
    WriteAt(&fixture.image, 0x100 + 2 * sizeof(Elf64_Sym), duplicate);
    fixture.view.sysv_hash_va = 0x1300;
    const std::array<uint32_t, 6> table = {1, 3, 1, 0, 2, 0};
    std::memcpy(fixture.image.data() + 0x300, table.data(), sizeof(table));

    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kFound),
              static_cast<uint32_t>(result.status));
    EXPECT_EQ(0x1600U, result.symbol.value);
}

TEST_CASE(ElfSysvHashLookupRejectsDistinctDuplicateSymbols) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    Elf64_Sym duplicate{};
    std::memcpy(&duplicate, fixture.image.data() + 0x100 + sizeof(Elf64_Sym), sizeof(duplicate));
    duplicate.st_value = 0x1700;
    WriteAt(&fixture.image, 0x100 + 2 * sizeof(Elf64_Sym), duplicate);
    fixture.view.sysv_hash_va = 0x1300;
    const std::array<uint32_t, 6> table = {1, 3, 1, 0, 2, 0};
    std::memcpy(fixture.image.data() + 0x300, table.data(), sizeof(table));

    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfGnuHashLookupRejectsDistinctDuplicateSymbols) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    Elf64_Sym duplicate{};
    std::memcpy(&duplicate, fixture.image.data() + 0x100 + sizeof(Elf64_Sym), sizeof(duplicate));
    duplicate.st_value = 0x1700;
    WriteAt(&fixture.image, 0x100 + 2 * sizeof(Elf64_Sym), duplicate);
    fixture.WriteGnuHash();
    const uint32_t hash = TestGnuHash("target");
    WriteAt(&fixture.image, 0x41c, hash & ~1U);
    WriteAt(&fixture.image, 0x420, hash | 1U);

    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfGnuHashLookupToleratesIdenticalDuplicateSymbols) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    Elf64_Sym duplicate{};
    std::memcpy(&duplicate, fixture.image.data() + 0x100 + sizeof(Elf64_Sym), sizeof(duplicate));
    WriteAt(&fixture.image, 0x100 + 2 * sizeof(Elf64_Sym), duplicate);
    fixture.WriteGnuHash();
    const uint32_t hash = TestGnuHash("target");
    WriteAt(&fixture.image, 0x41c, hash & ~1U);
    WriteAt(&fixture.image, 0x420, hash | 1U);

    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kFound),
              static_cast<uint32_t>(result.status));
    EXPECT_EQ(0x1600U, result.symbol.value);
}

TEST_CASE(ElfSysvHashRejectsWrappingTableAddressEvenWhenLowAddressIsReadable) {
    SparseElfReader reader;
    const uint64_t hash_va = UINT64_MAX - 7;
    const std::array<uint32_t, 2> header = {1, 1};
    reader.Write(hash_va, header);
    const uint32_t low_bucket = 0;
    reader.Write(0, low_bucket);
    dartplant::ElfDynamicView view{
        .symtab_va = 0x1000,
        .strtab_va = 0x1100,
        .strtab_size = 8,
        .symbol_entry_size = sizeof(Elf64_Sym),
        .sysv_hash_va = hash_va,
        .gnu_hash_va = std::nullopt,
    };

    const auto result = dartplant::FindElfDynamicSymbol(reader, view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfGnuHashRejectsWrappingTableAddressEvenWhenLowAddressIsReadable) {
    SparseElfReader reader;
    const uint64_t hash_va = UINT64_MAX - 15;
    const std::array<uint32_t, 4> header = {1, 1, 1, 5};
    reader.Write(hash_va, header);
    const Elf64_Addr low_bloom = UINT64_MAX;
    reader.Write(0, low_bloom);
    const uint32_t low_bucket = 0;
    reader.Write(sizeof(Elf64_Addr), low_bucket);
    dartplant::ElfDynamicView view{
        .symtab_va = 0x1000,
        .strtab_va = 0x1100,
        .strtab_size = 8,
        .symbol_entry_size = sizeof(Elf64_Sym),
        .sysv_hash_va = std::nullopt,
        .gnu_hash_va = hash_va,
    };

    const auto result = dartplant::FindElfDynamicSymbol(reader, view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfDynamicLookupRejectsWrappingSymbolTableIndex) {
    SparseElfReader reader;
    const std::array<uint32_t, 2> header = {1, 2};
    reader.Write(0x1000, header);
    const uint32_t symbol_index = 1;
    reader.Write(0x1008, symbol_index);
    dartplant::ElfDynamicView view{
        .symtab_va = UINT64_MAX - 15,
        .strtab_va = 0x1100,
        .strtab_size = 8,
        .symbol_entry_size = sizeof(Elf64_Sym),
        .sysv_hash_va = 0x1000,
        .gnu_hash_va = std::nullopt,
    };

    const auto result = dartplant::FindElfDynamicSymbol(reader, view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfDynamicLookupRejectsWrappingStringTableOffset) {
    SparseElfReader reader;
    const std::array<uint32_t, 2> header = {1, 2};
    reader.Write(0x1000, header);
    const uint32_t symbol_index = 1;
    reader.Write(0x1008, symbol_index);
    Elf64_Sym symbol{};
    symbol.st_name = 1;
    symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
    symbol.st_shndx = 1;
    symbol.st_value = 0x3000;
    reader.Write(0x2018, symbol);
    dartplant::ElfDynamicView view{
        .symtab_va = 0x2000,
        .strtab_va = UINT64_MAX,
        .strtab_size = 8,
        .symbol_entry_size = sizeof(Elf64_Sym),
        .sysv_hash_va = 0x1000,
        .gnu_hash_va = std::nullopt,
    };

    const auto result = dartplant::FindElfDynamicSymbol(reader, view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfSysvHashRejectsWrappingChainEntryAddress) {
    SparseElfReader reader;
    const uint64_t hash_va = UINT64_MAX - 15;
    const std::array<uint32_t, 2> header = {1, 2};
    reader.Write(hash_va, header);
    const uint32_t symbol_index = 1;
    reader.Write(UINT64_MAX - 7, symbol_index);
    Elf64_Sym symbol{};
    symbol.st_name = 0;
    symbol.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
    symbol.st_shndx = 1;
    symbol.st_value = 0x3000;
    reader.Write(0x2018, symbol);
    reader.Write(0x2100, 'o');
    reader.Write(0x2101, 't');
    reader.Write(0x2102, 'h');
    reader.Write(0x2103, 'e');
    reader.Write(0x2104, 'r');
    reader.Write(0x2105, '\0');
    const uint32_t low_chain = STN_UNDEF;
    reader.Write(0, low_chain);
    dartplant::ElfDynamicView view{
        .symtab_va = 0x2000,
        .strtab_va = 0x2100,
        .strtab_size = 6,
        .symbol_entry_size = sizeof(Elf64_Sym),
        .sysv_hash_va = hash_va,
        .gnu_hash_va = std::nullopt,
    };

    const auto result = dartplant::FindElfDynamicSymbol(reader, view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfDynamicLookupPrefersGnuWhenBothHashStylesExist) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    fixture.WriteSysvHash();
    fixture.WriteGnuHash();
    // Poison the SysV bucket. If lookup incorrectly tries SysV first this is
    // malformed, while the GNU path remains valid.
    const uint32_t impossible_sysv_symbol = 99;
    WriteAt(&fixture.image, 0x308, impossible_sysv_symbol);
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kFound),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfDynamicLookupRejectsStringOffsetAtOrBeyondStrsz) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol(static_cast<uint32_t>(fixture.view.strtab_size));
    fixture.WriteSysvHash();
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfDynamicLookupRejectsUnterminatedStringWithinStrsz) {
    DynamicLookupFixture fixture;
    fixture.view.strtab_size = 6;
    constexpr char kNoTerminator[] = "target";
    std::memcpy(fixture.image.data() + 0x200, kNoTerminator, sizeof(kNoTerminator) - 1);
    fixture.WriteTargetSymbol(0);
    // WriteTargetSymbol installed a normal string table; replace exactly the
    // advertised six bytes so no NUL exists inside DT_STRSZ.
    std::memcpy(fixture.image.data() + 0x200, kNoTerminator, sizeof(kNoTerminator) - 1);
    fixture.WriteSysvHash();
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfGnuHashRejectsBucketBeforeSymoffset) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    fixture.WriteGnuHash();
    const uint32_t symbol_offset = 2;
    WriteAt(&fixture.image, 0x404, symbol_offset);
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfGnuHashMalformedChainStopsAtReadablePtLoadBoundary) {
    DynamicLookupFixture fixture;
    fixture.WriteTargetSymbol();
    constexpr char kOtherNames[] = "\0other\0";
    std::memcpy(fixture.image.data() + 0x200, kOtherNames, sizeof(kOtherNames) - 1);
    fixture.WriteGnuHash(1, false);
    // Expose exactly one chain word through the PT_LOAD. A non-terminating
    // chain must become malformed rather than reading the next word from the
    // larger backing vector.
    fixture.headers[0].file_size = 0x420;
    fixture.headers[0].memory_size = 0x420;
    dartplant::FileElfReader reader(fixture.image, fixture.headers);
    const auto result = dartplant::FindElfDynamicSymbol(reader, fixture.view, "target");
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfDynamicLookupStatus::kMalformed),
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
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfProgramHeaderLookupStatus::kFound),
              static_cast<uint32_t>(parsed_dynamic.status));
    EXPECT_EQ(0x40U, parsed_dynamic.header.virtual_address);
}

TEST_CASE(ElfProgramHeaderParserValidatesLoadSegmentStructure) {
    const auto make_image = [](std::vector<Elf64_Phdr> program_headers) {
        std::vector<uint8_t> bytes(0x400, 0);
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
        header.e_phnum = static_cast<Elf64_Half>(program_headers.size());
        WriteAt(&bytes, 0, header);
        for (size_t index = 0; index < program_headers.size(); ++index) {
            WriteAt(&bytes, sizeof(Elf64_Ehdr) + index * sizeof(Elf64_Phdr),
                    program_headers[index]);
        }
        return bytes;
    };
    const auto valid_load = [] {
        Elf64_Phdr load{};
        load.p_type = PT_LOAD;
        load.p_flags = PF_R | PF_X;
        load.p_offset = 0x100;
        load.p_vaddr = 0x1000;
        load.p_filesz = 0x100;
        load.p_memsz = 0x200;
        load.p_align = 0x100;
        return load;
    };
    const auto parses = [&](Elf64_Phdr load) {
        std::vector<dartplant::ElfProgramHeaderView> parsed;
        const bool result = dartplant::ParseElf64ProgramHeaders(make_image({load}), &parsed);
        if (result) EXPECT_EQ(load.p_align, parsed[0].alignment);
        return result;
    };

    EXPECT_TRUE(parses(valid_load()));

    auto load = valid_load();
    load.p_filesz = load.p_memsz + 1;
    EXPECT_FALSE(parses(load));

    load = valid_load();
    load.p_offset = 0x3ff;
    load.p_filesz = 2;
    EXPECT_FALSE(parses(load));

    load = valid_load();
    load.p_offset = UINT64_MAX;
    load.p_filesz = 1;
    load.p_memsz = 1;
    load.p_align = 0;
    EXPECT_FALSE(parses(load));

    load = valid_load();
    load.p_vaddr = UINT64_MAX - 1;
    load.p_memsz = 2;
    load.p_filesz = 0;
    load.p_align = 0;
    EXPECT_FALSE(parses(load));

    load = valid_load();
    load.p_align = 3;
    EXPECT_FALSE(parses(load));

    load = valid_load();
    load.p_vaddr = 0x1001;
    EXPECT_FALSE(parses(load));
}

TEST_CASE(ElfProgramHeaderParserRejectsUnorderedOrOverlappingLoadSegments) {
    const auto make_image = [](Elf64_Phdr first, Elf64_Phdr second) {
        std::vector<uint8_t> bytes(0x400, 0);
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
        WriteAt(&bytes, 0, header);
        WriteAt(&bytes, sizeof(Elf64_Ehdr), first);
        WriteAt(&bytes, sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr), second);
        return bytes;
    };
    Elf64_Phdr first{};
    first.p_type = PT_LOAD;
    first.p_offset = 0x100;
    first.p_vaddr = 0x1000;
    first.p_filesz = 0x100;
    first.p_memsz = 0x200;
    Elf64_Phdr second = first;
    second.p_offset = 0x200;
    second.p_vaddr = 0x1100;
    second.p_filesz = 0x100;
    second.p_memsz = 0x100;

    std::vector<dartplant::ElfProgramHeaderView> parsed;
    EXPECT_FALSE(dartplant::ParseElf64ProgramHeaders(make_image(first, second), &parsed));

    second.p_vaddr = 0x800;
    second.p_offset = 0x300;
    EXPECT_FALSE(dartplant::ParseElf64ProgramHeaders(make_image(first, second), &parsed));

    second.p_vaddr = 0x1200;
    second.p_offset = 0x200;
    EXPECT_TRUE(dartplant::ParseElf64ProgramHeaders(make_image(first, second), &parsed));
}

TEST_CASE(ElfLoadLayoutValidationDoesNotRequireFileBacking) {
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .offset = 0x100000,
            .virtual_address = 0x200000,
            .file_size = 0x100,
            .memory_size = 0x200,
            .alignment = 0x100,
        },
    };
    EXPECT_TRUE(dartplant::ValidateElfLoadLayout(headers));
    EXPECT_FALSE(dartplant::ValidateElfLoadFileBounds({}, headers));
    EXPECT_FALSE(dartplant::ValidateElfLoadSegments({}, headers));
}

TEST_CASE(ElfProgramHeaderLookupDistinguishesMissingFromAmbiguousDynamicTable) {
    const std::array missing = {
        dartplant::ElfProgramHeaderView{.type = PT_LOAD},
    };
    const auto missing_result = dartplant::FindElfProgramHeader(missing, PT_DYNAMIC);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfProgramHeaderLookupStatus::kMissing),
              static_cast<uint32_t>(missing_result.status));

    const std::array duplicate = {
        dartplant::ElfProgramHeaderView{.type = PT_DYNAMIC, .virtual_address = 0x1000},
        dartplant::ElfProgramHeaderView{.type = PT_DYNAMIC, .virtual_address = 0x2000},
    };
    const auto duplicate_result = dartplant::FindElfProgramHeader(duplicate, PT_DYNAMIC);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfProgramHeaderLookupStatus::kAmbiguous),
              static_cast<uint32_t>(duplicate_result.status));
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

    header.e_machine = EM_AARCH64;
    WriteAt(&bytes, 0, header);
    EXPECT_FALSE(dartplant::ParseElf64ProgramHeaders(bytes, &parsed));
}
