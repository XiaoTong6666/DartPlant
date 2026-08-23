// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include "core/internal.h"
#include "test_runner.h"

TEST_CASE(ParseMetadataAcceptsDecimalAndHexOffsets) {
    constexpr char kMetadata[] = R"json({
    "format": 1,
    "module": {"soname": "libapp.so", "build_id": "abc123"},
    "methods": [
      {
        "library_uri": "package:fixture/main.dart",
        "class": "Fixture",
        "name": "add",
        "signature": "(int, int) -> int",
        "entry_kind": 0,
        "address_kind": 1,
        "code_offset": "0x1234",
        "code_size": 64,
        "fingerprint": "11223344"
      }
    ]
  })json";
    std::string error;
    const auto metadata = dartplant::ParseMetadata(kMetadata, &error);
    EXPECT_TRUE(metadata.has_value());
    EXPECT_EQ(1U, metadata->format);
    EXPECT_EQ(std::string("libapp.so"), metadata->module_name);
    EXPECT_EQ(1U, metadata->methods.size());
    EXPECT_EQ(0x1234ULL, metadata->methods[0].address);
}

TEST_CASE(ParseMetadataRejectsMissingMethods) {
    std::string error;
    const auto metadata = dartplant::ParseMetadata(
        R"json({"format":1,"module":{"soname":"libapp.so"},"methods":[]})json", &error);
    EXPECT_FALSE(metadata.has_value());
    EXPECT_FALSE(error.empty());
}

TEST_CASE(ParseMetadataRejectsMalformedJson) {
    std::string error;
    const auto metadata = dartplant::ParseMetadata("{not-json", &error);
    EXPECT_FALSE(metadata.has_value());
    EXPECT_FALSE(error.empty());
}
