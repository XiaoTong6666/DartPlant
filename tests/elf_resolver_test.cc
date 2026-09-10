// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>
#include <elf.h>

#include <array>
#include <cstring>
#include <vector>

#include "core/internal.h"
#include "elf/module_image.h"
#include "test_runner.h"

namespace {

template <typename T>
void AppendStruct(std::vector<uint8_t>* bytes, const T& value) {
    const size_t offset = bytes->size();
    bytes->resize(offset + sizeof(T));
    std::memcpy(bytes->data() + offset, &value, sizeof(T));
}

void AppendAlignedBytes(std::vector<uint8_t>* bytes, std::span<const uint8_t> data) {
    bytes->insert(bytes->end(), data.begin(), data.end());
    while ((bytes->size() & 3U) != 0) bytes->push_back(0);
}

void AppendNote(std::vector<uint8_t>* bytes, uint32_t type, std::span<const uint8_t> name,
                std::span<const uint8_t> descriptor) {
    Elf64_Nhdr header{};
    header.n_namesz = static_cast<uint32_t>(name.size());
    header.n_descsz = static_cast<uint32_t>(descriptor.size());
    header.n_type = type;
    AppendStruct(bytes, header);
    AppendAlignedBytes(bytes, name);
    AppendAlignedBytes(bytes, descriptor);
}

}  // namespace

TEST_CASE(EnumerateModulesFindsLoadedFixture) {
    void* handle = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(handle != nullptr);
    void* symbol = dlsym(handle, "DartPlantFixtureAdd");
    EXPECT_TRUE(symbol != nullptr);

    const auto modules = dartplant::EnumerateModules();
    const auto module = dartplant::FindModule(modules, "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());
    EXPECT_TRUE(module->load_bias != 0);
    EXPECT_TRUE(module->ContainsExecutable(reinterpret_cast<uintptr_t>(symbol), 1));

    const uintptr_t address = reinterpret_cast<uintptr_t>(symbol);
    const auto resolved = module->Resolve(DARTPLANT_ADDRESS_ELF_VA, address - module->load_bias);
    EXPECT_TRUE(resolved.has_value());
    EXPECT_EQ(address, *resolved);
    dlclose(handle);
}

TEST_CASE(FingerprintIsStableAndSensitive) {
    const unsigned char first[] = {1, 2, 3, 4};
    const unsigned char second[] = {1, 2, 3, 5};
    EXPECT_EQ(dartplant::FingerprintCode(first, sizeof(first)),
              dartplant::FingerprintCode(first, sizeof(first)));
    EXPECT_FALSE(dartplant::FingerprintCode(first, sizeof(first)) ==
                 dartplant::FingerprintCode(second, sizeof(second)));
}

TEST_CASE(ElfProgramHeaderCorpusAcceptsMergedRxAtZeroAndApkDirectMapping) {
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0,
            .virtual_address = 0,
            .file_size = 0x1800,
            .memory_size = 0x2000,
        },
    };
    dartplant::ModuleImage image;
    EXPECT_TRUE(dartplant::BuildModuleImageFromProgramHeaders(
        "/data/app/example/base.apk!/lib/arm64-v8a/libapp.so", 0x100000, headers, &image));
    EXPECT_TRUE(image.name == "libapp.so");
    EXPECT_TRUE(image.build_id.empty());
    EXPECT_EQ(1U, image.executable_ranges.size());
    EXPECT_EQ(0x100000U, image.executable_ranges[0].start);
    EXPECT_EQ(0x102000U, image.executable_ranges[0].end);
    EXPECT_TRUE(image.ContainsExecutable(0x101f00, 0x100));
    const auto file_address = image.Resolve(DARTPLANT_ADDRESS_FILE_OFFSET, 0x100);
    EXPECT_TRUE(file_address.has_value());
    EXPECT_EQ(0x100100U, *file_address);
}

TEST_CASE(ElfProgramHeaderCorpusKeepsAllExecutableLoadsInVaOrder) {
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R,
            .offset = 0,
            .virtual_address = 0,
            .file_size = 0x1000,
            .memory_size = 0x1000,
        },
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0x4000,
            .virtual_address = 0x5000,
            .file_size = 0x800,
            .memory_size = 0x1000,
        },
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0x2000,
            .virtual_address = 0x3000,
            .file_size = 0x1000,
            .memory_size = 0x1000,
        },
    };
    dartplant::ModuleImage image;
    EXPECT_TRUE(dartplant::BuildModuleImageFromProgramHeaders("/tmp/libmulti.so", 0x200000, headers,
                                                              &image));
    EXPECT_EQ(2U, image.executable_ranges.size());
    EXPECT_EQ(0x3000U, image.executable_ranges[0].virtual_address);
    EXPECT_EQ(0x5000U, image.executable_ranges[1].virtual_address);
    EXPECT_TRUE(image.ContainsExecutable(0x203000, 4));
    EXPECT_TRUE(image.ContainsExecutable(0x205000, 4));
}

TEST_CASE(ElfProgramHeaderCorpusSeparatesFileSizeFromExecutableMemorySize) {
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0x1000,
            .virtual_address = 0x2000,
            .file_size = 0x400,
            .memory_size = 0x800,
        },
    };
    dartplant::ModuleImage image;
    EXPECT_TRUE(
        dartplant::BuildModuleImageFromProgramHeaders("libtail.so", 0x300000, headers, &image));
    EXPECT_TRUE(image.ContainsExecutable(0x302700, 0x100));
    EXPECT_TRUE(image.Resolve(DARTPLANT_ADDRESS_FILE_OFFSET, 0x13ff).has_value());
    EXPECT_FALSE(image.Resolve(DARTPLANT_ADDRESS_FILE_OFFSET, 0x1400).has_value());
}

TEST_CASE(ElfFileOffsetResolutionRejectsAmbiguousExecutableMappings) {
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0x1000,
            .virtual_address = 0x4000,
            .file_size = 0x1000,
            .memory_size = 0x1000,
        },
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0x1800,
            .virtual_address = 0x8000,
            .file_size = 0x1000,
            .memory_size = 0x1000,
        },
    };
    dartplant::ModuleImage image;
    EXPECT_TRUE(dartplant::BuildModuleImageFromProgramHeaders("libambiguous.so", 0x100000, headers,
                                                              &image));
    EXPECT_FALSE(image.Resolve(DARTPLANT_ADDRESS_FILE_OFFSET, 0x1900).has_value());
    EXPECT_TRUE(image.Resolve(DARTPLANT_ADDRESS_FILE_OFFSET, 0x1100).has_value());
}

TEST_CASE(ElfProgramHeaderCorpusRejectsOverflowAndImpossibleFileSize) {
    const std::array overflow = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0,
            .virtual_address = UINT64_MAX,
            .file_size = 4,
            .memory_size = 4,
        },
    };
    dartplant::ModuleImage image;
    EXPECT_FALSE(
        dartplant::BuildModuleImageFromProgramHeaders("overflow.so", 0x1000, overflow, &image));

    const std::array bad_size = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0,
            .virtual_address = 0,
            .file_size = 0x2000,
            .memory_size = 0x1000,
        },
    };
    EXPECT_FALSE(dartplant::BuildModuleImageFromProgramHeaders("bad-size.so", 0, bad_size, &image));
}

TEST_CASE(SnapshotOffsetRequiresExplicitIsolateInstructionsBase) {
    const std::array headers = {
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0x1000,
            .virtual_address = 0x1000,
            .file_size = 0x1000,
            .memory_size = 0x1000,
        },
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0x3000,
            .virtual_address = 0x3000,
            .file_size = 0x1000,
            .memory_size = 0x1000,
        },
        dartplant::ElfProgramHeaderView{
            .type = PT_LOAD,
            .flags = PF_R | PF_X,
            .offset = 0x7000,
            .virtual_address = 0x7000,
            .file_size = 0x2000,
            .memory_size = 0x2000,
        },
    };
    dartplant::ModuleImage image;
    EXPECT_TRUE(
        dartplant::BuildModuleImageFromProgramHeaders("libapp.so", 0x100000, headers, &image));
    EXPECT_FALSE(image.Resolve(DARTPLANT_ADDRESS_SNAPSHOT_OFFSET, 0x100).has_value());
}

TEST_CASE(ElfBuildIdParserHandlesPaddingAndMultipleNotes) {
    std::vector<uint8_t> notes;
    const std::array<uint8_t, 4> gnu_name = {'G', 'N', 'U', 0};
    const std::array<uint8_t, 3> other_name = {'X', 'Y', 0};
    const std::array<uint8_t, 3> other_desc = {1, 2, 3};
    const std::array<uint8_t, 5> build_id = {0xaa, 0xbb, 0x00, 0x11, 0x22};
    AppendNote(&notes, 1, other_name, other_desc);
    AppendNote(&notes, 3, gnu_name, build_id);

    const auto result = dartplant::ParseGnuBuildIdNotes(notes);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kFound),
              static_cast<uint32_t>(result.status));
    EXPECT_EQ(std::string("aabb001122"), result.build_id);
}

TEST_CASE(ElfBuildIdParserRequiresTheCompleteGnuOwner) {
    std::vector<uint8_t> notes;
    const std::array<uint8_t, 4> wrong_owner = {'G', 'N', 'U', 'X'};
    const std::array<uint8_t, 2> build_id = {0xaa, 0xbb};
    AppendNote(&notes, 3, wrong_owner, build_id);

    const auto result = dartplant::ParseGnuBuildIdNotes(notes);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kNotFound),
              static_cast<uint32_t>(result.status));
}

TEST_CASE(ElfBuildIdParserToleratesRepeatedIdenticalBuildIds) {
    std::vector<uint8_t> notes;
    const std::array<uint8_t, 4> gnu_name = {'G', 'N', 'U', 0};
    const std::array<uint8_t, 3> build_id = {0xaa, 0xbb, 0xcc};
    AppendNote(&notes, 3, gnu_name, build_id);
    AppendNote(&notes, 3, gnu_name, build_id);

    const auto result = dartplant::ParseGnuBuildIdNotes(notes);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kFound),
              static_cast<uint32_t>(result.status));
    EXPECT_EQ(std::string("aabbcc"), result.build_id);
}

TEST_CASE(ElfBuildIdParserRejectsDistinctBuildIdsAsAmbiguous) {
    std::vector<uint8_t> notes;
    const std::array<uint8_t, 4> gnu_name = {'G', 'N', 'U', 0};
    const std::array<uint8_t, 3> first = {0xaa, 0xbb, 0xcc};
    const std::array<uint8_t, 3> second = {0x11, 0x22, 0x33};
    AppendNote(&notes, 3, gnu_name, first);
    AppendNote(&notes, 3, gnu_name, second);

    const auto result = dartplant::ParseGnuBuildIdNotes(notes);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kAmbiguous),
              static_cast<uint32_t>(result.status));
    EXPECT_TRUE(result.build_id.empty());
}

TEST_CASE(ElfBuildIdResultsAggregateAcrossNoteSegments) {
    const std::array<uint8_t, 4> gnu_name = {'G', 'N', 'U', 0};
    const std::array<uint8_t, 2> first_id = {0xaa, 0xbb};
    const std::array<uint8_t, 2> second_id = {0x11, 0x22};
    std::vector<uint8_t> first_notes;
    std::vector<uint8_t> same_notes;
    std::vector<uint8_t> second_notes;
    AppendNote(&first_notes, 3, gnu_name, first_id);
    AppendNote(&same_notes, 3, gnu_name, first_id);
    AppendNote(&second_notes, 3, gnu_name, second_id);

    const auto not_found = dartplant::ElfBuildIdResult{
        .status = dartplant::ElfBuildIdStatus::kNotFound,
        .build_id = {},
    };
    const auto first = dartplant::ParseGnuBuildIdNotes(first_notes);
    const auto same = dartplant::ParseGnuBuildIdNotes(same_notes);
    const auto second = dartplant::ParseGnuBuildIdNotes(second_notes);
    const auto found = dartplant::MergeElfBuildIdResults(not_found, first);
    const auto repeated = dartplant::MergeElfBuildIdResults(found, same);
    const auto ambiguous = dartplant::MergeElfBuildIdResults(repeated, second);

    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kFound),
              static_cast<uint32_t>(repeated.status));
    EXPECT_EQ(std::string("aabb"), repeated.build_id);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kAmbiguous),
              static_cast<uint32_t>(ambiguous.status));
    EXPECT_TRUE(ambiguous.build_id.empty());
}

TEST_CASE(ElfLoadedBuildIdUsesFileszAndReadableLoadContainment) {
    const std::array<uint8_t, 4> gnu_name = {'G', 'N', 'U', 0};
    const std::array<uint8_t, 2> first_id = {0xaa, 0xbb};
    const std::array<uint8_t, 2> second_id = {0x11, 0x22};
    std::vector<uint8_t> first_notes;
    std::vector<uint8_t> all_notes;
    AppendNote(&first_notes, 3, gnu_name, first_id);
    all_notes = first_notes;
    AppendNote(&all_notes, 3, gnu_name, second_id);

    const auto make_headers = [&](uint32_t load_flags, uint64_t load_va, uint64_t note_size,
                                  uint64_t note_memory_size) {
        return std::array<dartplant::ElfProgramHeaderView, 2>{
            dartplant::ElfProgramHeaderView{
                .type = PT_LOAD,
                .flags = load_flags,
                .offset = 0,
                .virtual_address = load_va,
                .file_size = all_notes.size(),
                .memory_size = all_notes.size(),
            },
            dartplant::ElfProgramHeaderView{
                .type = PT_NOTE,
                .flags = 0,
                .offset = 0,
                .virtual_address = 0,
                .file_size = note_size,
                .memory_size = note_memory_size,
            },
        };
    };
    const uintptr_t load_bias = reinterpret_cast<uintptr_t>(all_notes.data());
    const auto valid = make_headers(PF_R, 0, first_notes.size(), all_notes.size());
    const auto result = dartplant::ReadLoadedGnuBuildIdNotes(load_bias, valid);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kFound),
              static_cast<uint32_t>(result.status));
    EXPECT_EQ(std::string("aabb"), result.build_id);

    const auto unreadable = make_headers(PF_X, 0, first_notes.size(), all_notes.size());
    EXPECT_EQ(
        static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kMalformed),
        static_cast<uint32_t>(dartplant::ReadLoadedGnuBuildIdNotes(load_bias, unreadable).status));

    const auto outside = make_headers(PF_R, 0x1000, first_notes.size(), all_notes.size());
    EXPECT_EQ(
        static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kMalformed),
        static_cast<uint32_t>(dartplant::ReadLoadedGnuBuildIdNotes(load_bias, outside).status));

    const auto oversized = make_headers(PF_R, 0, all_notes.size(), first_notes.size());
    EXPECT_EQ(
        static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kMalformed),
        static_cast<uint32_t>(dartplant::ReadLoadedGnuBuildIdNotes(load_bias, oversized).status));

    auto bss_tail = make_headers(PF_R, 0, all_notes.size(), all_notes.size());
    bss_tail[0].file_size = first_notes.size();
    EXPECT_EQ(
        static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kMalformed),
        static_cast<uint32_t>(dartplant::ReadLoadedGnuBuildIdNotes(load_bias, bss_tail).status));

    auto offset_mismatch = valid;
    offset_mismatch[1].offset = 0x900;
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kMalformed),
              static_cast<uint32_t>(
                  dartplant::ReadLoadedGnuBuildIdNotes(load_bias, offset_mismatch).status));
}

TEST_CASE(ElfBuildIdParserRejectsAdversarialNoteLengthsWithoutOverflow) {
    std::vector<uint8_t> notes(sizeof(Elf64_Nhdr), 0);
    Elf64_Nhdr header{};
    header.n_namesz = UINT32_MAX;
    header.n_descsz = UINT32_MAX;
    header.n_type = 3;
    std::memcpy(notes.data(), &header, sizeof(header));

    const auto result = dartplant::ParseGnuBuildIdNotes(notes);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kMalformed),
              static_cast<uint32_t>(result.status));
    EXPECT_TRUE(result.build_id.empty());
}

TEST_CASE(ElfBuildIdParserRejectsNonPaddingTruncatedTail) {
    std::vector<uint8_t> notes = {0x7f};
    const auto result = dartplant::ParseGnuBuildIdNotes(notes);
    EXPECT_EQ(static_cast<uint32_t>(dartplant::ElfBuildIdStatus::kMalformed),
              static_cast<uint32_t>(result.status));
}
