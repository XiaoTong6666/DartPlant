// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "runtime/runtime_image_set.h"

#include <array>

#include "test_runner.h"

namespace {

dartplant::ModuleImage MakeModule(const char* name, const char* path, const char* build_id,
                                  uintptr_t load_bias, uintptr_t instructions,
                                  uint64_t instructions_va, uint64_t instructions_size) {
    dartplant::ModuleImage module;
    module.name = name;
    module.path = path;
    module.build_id = build_id;
    module.load_bias = load_bias;
    module.executable_ranges.push_back({
        .start = instructions,
        .end = instructions + instructions_size,
        .file_offset = instructions_va,
        .virtual_address = instructions_va,
        .file_size = instructions_size,
    });
    return module;
}

dartplant::FlutterSnapshotSource MakeSnapshot(
    const dartplant::ModuleImage& module, uintptr_t instructions, uint64_t instructions_va,
    uint64_t instructions_size, std::optional<uint32_t> deferred_program_hash = std::nullopt) {
    dartplant::FlutterSnapshotSource snapshot;
    snapshot.module_name = module.name;
    snapshot.module_path = module.path;
    snapshot.module_build_id = module.build_id;
    snapshot.snapshot_hash = "0123456789abcdef0123456789abcdef";
    snapshot.snapshot_features = "arm64 android product compressed-pointers";
    snapshot.profile_name = "flutter-arm64-product-compressed";
    snapshot.isolate_instructions_va = instructions_va;
    snapshot.isolate_instructions_size = instructions_size;
    snapshot.isolate_instructions_runtime = instructions;
    snapshot.compressed_pointers = true;
    snapshot.deferred_program_hash = deferred_program_hash;
    return snapshot;
}

}  // namespace

TEST_CASE(RuntimeImageSetMapsRootAndDeferredEntryNamespacesIndependently) {
    const auto root_module =
        MakeModule("libapp.so", "/base/libapp.so", "aaaa", 0x100000, 0x110000, 0x10000, 0x2000);
    const auto unit_module = MakeModule("libapp.so-2.part.so", "/feature/libapp.so-2.part.so",
                                        "bbbb", 0x200000, 0x220000, 0x20000, 0x3000);
    const auto root_snapshot = MakeSnapshot(root_module, 0x110000, 0x10000, 0x2000);
    const auto unit_snapshot = MakeSnapshot(unit_module, 0x220000, 0x20000, 0x3000, 0x12345678);

    dartplant::RuntimeImageSet images;
    std::string error;
    EXPECT_TRUE(images.SetRoot(root_module, root_snapshot, 7, &error));
    EXPECT_TRUE(images.AddDeferred(unit_module, unit_snapshot, 2, 7, &error));
    EXPECT_EQ(2U, images.size());

    const auto* root = images.FindByRuntimeRange(0x110080, 4);
    const auto* unit = images.FindByRuntimeRange(0x220100, 4);
    EXPECT_TRUE(root != nullptr);
    EXPECT_TRUE(unit != nullptr);
    EXPECT_TRUE(root->id != unit->id);
    EXPECT_EQ(1U, root->loading_unit_id);
    EXPECT_EQ(2U, unit->loading_unit_id);
    EXPECT_EQ(0x10080U, root->RuntimeAddressToElfVa(0x110080, 4).value_or(0));
    EXPECT_EQ(0x20100U, unit->RuntimeAddressToElfVa(0x220100, 4).value_or(0));
}

TEST_CASE(RuntimeImageSetRejectsOverlappingInstructionNamespaces) {
    const auto root_module =
        MakeModule("libapp.so", "/base/libapp.so", "aaaa", 0x100000, 0x110000, 0x10000, 0x2000);
    const auto root_snapshot = MakeSnapshot(root_module, 0x110000, 0x10000, 0x2000);
    const auto unit_module = MakeModule("libapp.so-2.part.so", "/feature/libapp.so-2.part.so",
                                        "bbbb", 0x100000, 0x111000, 0x20000, 0x2000);
    const auto unit_snapshot = MakeSnapshot(unit_module, 0x111000, 0x20000, 0x2000, 0x12345678);

    dartplant::RuntimeImageSet images;
    std::string error;
    EXPECT_TRUE(images.SetRoot(root_module, root_snapshot, 1, &error));
    EXPECT_TRUE(!images.AddDeferred(unit_module, unit_snapshot, 2, 1, &error));
}

TEST_CASE(RuntimeImageSetRejectsDuplicateLoadingUnitIds) {
    const auto root_module =
        MakeModule("libapp.so", "/base/libapp.so", "aaaa", 0x100000, 0x110000, 0x10000, 0x1000);
    const auto root_snapshot = MakeSnapshot(root_module, 0x110000, 0x10000, 0x1000);
    const auto unit2a = MakeModule("libapp.so-2.part.so", "/a/libapp.so-2.part.so", "bbbb",
                                   0x200000, 0x210000, 0x10000, 0x1000);
    const auto unit2b = MakeModule("libapp-2.part.so", "/b/libapp-2.part.so", "cccc", 0x300000,
                                   0x310000, 0x10000, 0x1000);

    dartplant::RuntimeImageSet images;
    std::string error;
    EXPECT_TRUE(images.SetRoot(root_module, root_snapshot, 1, &error));
    EXPECT_TRUE(images.AddDeferred(
        unit2a, MakeSnapshot(unit2a, 0x210000, 0x10000, 0x1000, 0x12345678), 2, 1, &error));
    EXPECT_TRUE(!images.AddDeferred(
        unit2b, MakeSnapshot(unit2b, 0x310000, 0x10000, 0x1000, 0x12345678), 2, 1, &error));
}

TEST_CASE(RuntimeImageSetParsesDartAndFlutterDeferredUnitNames) {
    EXPECT_EQ(
        2U, dartplant::ParseDeferredLoadingUnitId("libapp.so", "libapp.so-2.part.so").value_or(0));
    EXPECT_EQ(37U,
              dartplant::ParseDeferredLoadingUnitId("libapp.so", "libapp-37.part.so").value_or(0));
    EXPECT_TRUE(!dartplant::ParseDeferredLoadingUnitId("libapp.so", "libapp.so").has_value());
    EXPECT_TRUE(
        !dartplant::ParseDeferredLoadingUnitId("libapp.so", "libapp.so-1.part.so").has_value());
    EXPECT_TRUE(!dartplant::ParseDeferredLoadingUnitId("libapp.so", "other-2.part.so").has_value());
}

TEST_CASE(RuntimeImageSetPublishesLiveSemanticBindingsTransactionally) {
    const auto root_module =
        MakeModule("libapp.so", "/base/libapp.so", "aaaa", 0x100000, 0x110000, 0x10000, 0x1000);
    const auto unit_module = MakeModule("libapp.so-2.part.so", "/feature/libapp.so-2.part.so",
                                        "bbbb", 0x200000, 0x210000, 0x10000, 0x1000);
    dartplant::RuntimeImageSet images;
    std::string error;
    EXPECT_TRUE(images.SetRoot(root_module, MakeSnapshot(root_module, 0x110000, 0x10000, 0x1000),
                               11, &error));
    EXPECT_TRUE(images.AddDeferred(unit_module,
                                   MakeSnapshot(unit_module, 0x210000, 0x10000, 0x1000, 0x12345678),
                                   2, 11, &error));
    const auto* root = images.Root();
    const auto* unit = images.FindByLoadingUnitId(2);
    EXPECT_TRUE(root != nullptr);
    EXPECT_TRUE(unit != nullptr);
    const dartplant::RuntimeImageId root_id = root->id;
    const dartplant::RuntimeImageId unit_id = unit->id;

    const std::array<dartplant::RuntimeImageId, 3> bindings = {root_id, unit_id, unit_id};
    EXPECT_TRUE(images.BindLiveEntries(bindings));
    EXPECT_EQ(1U, images.FindById(root_id)->live_entry_count);
    EXPECT_EQ(2U, images.FindById(unit_id)->live_entry_count);

    const std::array<dartplant::RuntimeImageId, 2> malformed = {root_id, 0xfeedbeefULL};
    EXPECT_TRUE(!images.BindLiveEntries(malformed));
    EXPECT_EQ(1U, images.FindById(root_id)->live_entry_count);
    EXPECT_EQ(2U, images.FindById(unit_id)->live_entry_count);

    const std::array<dartplant::RuntimeImageId, 1> rebound = {unit_id};
    EXPECT_TRUE(images.BindLiveEntries(rebound));
    EXPECT_EQ(0U, images.FindById(root_id)->live_entry_count);
    EXPECT_EQ(1U, images.FindById(unit_id)->live_entry_count);
}

TEST_CASE(RuntimeImageSetRejectsDeferredUnitsFromDifferentProgramHashes) {
    const auto root_module =
        MakeModule("libapp.so", "/base/libapp.so", "aaaa", 0x100000, 0x110000, 0x10000, 0x1000);
    const auto unit2 = MakeModule("libapp.so-2.part.so", "/feature/libapp.so-2.part.so", "bbbb",
                                  0x200000, 0x210000, 0x10000, 0x1000);
    const auto unit3 = MakeModule("libapp.so-3.part.so", "/feature/libapp.so-3.part.so", "cccc",
                                  0x300000, 0x310000, 0x10000, 0x1000);
    dartplant::RuntimeImageSet images;
    std::string error;
    EXPECT_TRUE(images.SetRoot(root_module, MakeSnapshot(root_module, 0x110000, 0x10000, 0x1000), 5,
                               &error));
    EXPECT_TRUE(images.AddDeferred(
        unit2, MakeSnapshot(unit2, 0x210000, 0x10000, 0x1000, 0x11111111), 2, 5, &error));
    EXPECT_TRUE(!images.AddDeferred(
        unit3, MakeSnapshot(unit3, 0x310000, 0x10000, 0x1000, 0x22222222), 3, 5, &error));
    EXPECT_EQ(std::string("deferred loading units disagree on Dart program hash"), error);
}

TEST_CASE(RuntimeImageSetBindsDeferredProgramHashTransactionallyToLiveRoot) {
    const auto root_module =
        MakeModule("libapp.so", "/base/libapp.so", "aaaa", 0x100000, 0x110000, 0x10000, 0x1000);
    const auto unit2 = MakeModule("libapp.so-2.part.so", "/feature/libapp.so-2.part.so", "bbbb",
                                  0x200000, 0x210000, 0x10000, 0x1000);
    dartplant::RuntimeImageSet images;
    std::string error;
    EXPECT_TRUE(images.SetRoot(root_module, MakeSnapshot(root_module, 0x110000, 0x10000, 0x1000), 5,
                               &error));
    EXPECT_TRUE(images.AddDeferred(
        unit2, MakeSnapshot(unit2, 0x210000, 0x10000, 0x1000, 0x12345678), 2, 5, &error));
    EXPECT_TRUE(!images.FindByLoadingUnitId(2)->deferred_program_hash_vm_bound);

    EXPECT_TRUE(!images.BindDeferredProgramHash(0x87654321));
    EXPECT_TRUE(!images.FindByLoadingUnitId(2)->deferred_program_hash_vm_bound);

    EXPECT_TRUE(images.BindDeferredProgramHash(0x12345678));
    EXPECT_TRUE(images.FindByLoadingUnitId(2)->deferred_program_hash_vm_bound);
}
