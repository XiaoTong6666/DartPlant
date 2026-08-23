#include "runtime/flutter_snapshot_internal.h"
#include "test_runner.h"

TEST_CASE(FlutterSnapshotProfileSelectsCurrentArm64ProductLayout) {
    const auto profile =
        dartplant::SelectFlutterSnapshotProfile("arm64 product compressed-pointers CID_SHIFT1");
    EXPECT_TRUE(profile.has_value());
    EXPECT_EQ(std::string("flutter-arm64-product-compressed"), *profile);
}

TEST_CASE(FlutterSnapshotProfileRejectsUnknownLayout) {
    const auto profile = dartplant::SelectFlutterSnapshotProfile("arm64 experimental-layout");
    EXPECT_FALSE(profile.has_value());
}
