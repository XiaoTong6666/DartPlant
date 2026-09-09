#include <cstring>
#include <vector>

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
