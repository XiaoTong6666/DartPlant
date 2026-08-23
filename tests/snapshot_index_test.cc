#include "runtime/snapshot_index.h"

#include "core/internal.h"
#include "test_runner.h"

TEST_CASE(SnapshotIndexFindsExactFunctionIdentity) {
    dartplant::SnapshotIndex index;
    index.functions.push_back({
        .library_uri = "package:app/main.dart",
        .class_name = "Global",
        .function_name = "instrumentedAdd",
        .signature = "",
        .entry_va = 0x256c44,
        .code_size = 76,
        .code_section_va = 0x160000,
        .fingerprint = "7a70bd3ea3c7204d",
    });

    const auto* function = index.FindSnapshotFunction(
        "package:app/main.dart", "Global", "instrumentedAdd", "", DARTPLANT_ENTRY_DEFAULT);
    EXPECT_TRUE(function != nullptr);
    EXPECT_EQ(0x256c44ULL, function->entry_va);
    EXPECT_EQ(76ULL, function->code_size);
    EXPECT_EQ(0x160000ULL, function->code_section_va);
    EXPECT_EQ(std::string("7a70bd3ea3c7204d"), function->fingerprint);
}

TEST_CASE(SnapshotIndexRejectsAmbiguousLiveIdentity) {
    dartplant::SnapshotIndex index;
    dartplant::SnapshotFunction function;
    function.library_uri = "package:app/main.dart";
    function.class_name = "Fixture";
    function.function_name = "add";
    function.signature = "";
    function.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    function.runtime_entry = 0x1000;
    function.live = true;
    index.functions.push_back(function);
    function.runtime_entry = 0x2000;
    index.functions.push_back(function);

    bool ambiguous = false;
    EXPECT_TRUE(index.FindSnapshotFunction("package:app/main.dart", "Fixture", "add", "",
                                           DARTPLANT_ENTRY_DEFAULT, &ambiguous) == nullptr);
    EXPECT_TRUE(ambiguous);
}

TEST_CASE(SnapshotIndexRequiresAllQueryFields) {
    dartplant::SnapshotIndex index;
    index.functions.push_back({
        .library_uri = "package:app/main.dart",
        .class_name = "Global",
        .function_name = "instrumentedAdd",
        .signature = "",
        .entry_va = 0,
        .code_size = 0,
        .code_section_va = 0,
        .fingerprint = "",
    });
    EXPECT_TRUE(index.FindSnapshotFunction("package:app/main.dart", "Global", "instrumentedAdd",
                                           "(int, int) -> int",
                                           DARTPLANT_ENTRY_DEFAULT) == nullptr);
}

TEST_CASE(SnapshotIndexCopiesExternalRecords) {
    const char* library = "package:app/main.dart";
    const char* function_name = "instrumentedAdd";
    const char* signature = "";
    const char* fingerprint = "7a70bd3ea3c7204d";
    DartPlantSnapshotFunctionInfo function{};
    function.struct_size = sizeof(function);
    function.library_uri = library;
    function.class_name = "Global";
    function.function_name = function_name;
    function.signature = signature;
    function.entry_kind = DARTPLANT_ENTRY_DEFAULT;
    function.entry_va = 0x256c44;
    function.code_size = 76;
    function.code_section_va = 0x250000;
    function.fingerprint = fingerprint;

    DartPlantSnapshotIndexInfo source{};
    source.struct_size = sizeof(source);
    source.module_name = "libapp.so";
    source.module_build_id = "deadbeef";
    source.snapshot_hash = "0123456789abcdef0123456789abcdef";
    source.dart_version = "3.x";
    source.profile_version = "flutter-arm64-product-compressed";
    source.functions = &function;
    source.function_count = 1;
    std::string error;
    const auto index = dartplant::BuildSnapshotIndex(source, &error);
    EXPECT_TRUE(index.has_value());
    EXPECT_EQ(std::string("libapp.so"), index->module_name);
    const auto* copied = index->FindSnapshotFunction(library, "Global", function_name, signature,
                                                     DARTPLANT_ENTRY_DEFAULT);
    EXPECT_TRUE(copied != nullptr);
    EXPECT_EQ(std::string(fingerprint), copied->fingerprint);
}
