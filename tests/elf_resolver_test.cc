// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>
#include <elf.h>

#include <array>

#include "core/internal.h"
#include "test_runner.h"

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
