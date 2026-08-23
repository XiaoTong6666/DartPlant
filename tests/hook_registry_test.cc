// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>

#include "core/internal.h"
#include "test_runner.h"

namespace {

int g_hook_calls = 0;
int g_unhook_calls = 0;
void* g_last_target = nullptr;

int FakeHook(void* target, void*, void** backup) {
    ++g_hook_calls;
    g_last_target = target;
    *backup = target;
    return 0;
}

int FakeUnhook(void* target) {
    ++g_unhook_calls;
    g_last_target = target;
    return 0;
}

int Replacement(int, int) { return 42; }

void ResetFakeHost() {
    dartplant_reset();
    g_hook_calls = 0;
    g_unhook_calls = 0;
    g_last_target = nullptr;
    const DartPlantNativeApiEntries entries = {2, FakeHook, FakeUnhook};
    dartplant::InstallHostApi(&entries);
    dartplant::RefreshModules();
}

}  // namespace

TEST_CASE(AddressHookUsesHostApiAndUnhooks) {
    ResetFakeHost();
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();

    DartPlantAddressQuery query = {
        .struct_size = sizeof(query),
        .module_name = "libdartplant_fixture.so",
        .address = reinterpret_cast<uint64_t>(target),
        .address_kind = DARTPLANT_ADDRESS_RUNTIME,
        .code_size = 1,
        .expected_build_id = nullptr,
        .expected_fingerprint = nullptr,
    };
    void* backup = nullptr;
    DartPlantHook* hook = nullptr;
    EXPECT_EQ(DARTPLANT_OK,
              dartplant_hook_address(&query, reinterpret_cast<void*>(Replacement), &backup, &hook));
    EXPECT_EQ(1, g_hook_calls);
    EXPECT_EQ(target, backup);
    EXPECT_EQ(target, g_last_target);
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(hook));
    EXPECT_EQ(1, g_unhook_calls);
    dartplant_release_hook(hook);
    dlclose(fixture);
}

TEST_CASE(MethodQueryResolvesFixtureAddress) {
    ResetFakeHost();
    void* fixture = dlopen(DARTPLANT_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    EXPECT_TRUE(fixture != nullptr);
    void* target = dlsym(fixture, "DartPlantFixtureAdd");
    EXPECT_TRUE(target != nullptr);
    dartplant::RefreshModules();
    const auto modules = dartplant::EnumerateModules();
    const auto module = dartplant::FindModule(modules, "libdartplant_fixture.so");
    EXPECT_TRUE(module.has_value());

    const uint64_t elf_va = reinterpret_cast<uintptr_t>(target) - module->load_bias;
    const std::string metadata =
        "{\"format\":1,\"module\":{\"soname\":\"libdartplant_fixture.so\"},"
        "\"methods\":[{\"library_uri\":\"package:fixture/main.dart\","
        "\"class\":\"Fixture\",\"name\":\"add\","
        "\"entry_kind\":0,"
        "\"address_kind\":1,\"code_offset\":" +
        std::to_string(elf_va) + ",\"code_size\":1}]}";
    EXPECT_EQ(DARTPLANT_OK, dartplant_initialize_from_json(metadata.c_str()));

    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:fixture/main.dart",
        .class_name = "Fixture",
        .function_name = "add",
        .signature = "(int, int) -> int",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_OK, dartplant_find_method(&query, &method));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(target), dartplant_method_runtime_address(method));
    dartplant_release_method(method);
    dartplant_reset();
    dlclose(fixture);
}

TEST_CASE(AddressHookRejectsNonExecutableRange) {
    ResetFakeHost();
    int stack_value = 0;
    DartPlantAddressQuery query = {
        .struct_size = sizeof(query),
        .module_name = "libc.so.6",
        .address = reinterpret_cast<uint64_t>(&stack_value),
        .address_kind = DARTPLANT_ADDRESS_RUNTIME,
        .code_size = sizeof(stack_value),
        .expected_build_id = nullptr,
        .expected_fingerprint = nullptr,
    };
    void* backup = nullptr;
    DartPlantHook* hook = nullptr;
    EXPECT_EQ(DARTPLANT_ADDRESS_OUTSIDE_EXECUTABLE,
              dartplant_hook_address(&query, reinterpret_cast<void*>(Replacement), &backup, &hook));
    EXPECT_EQ(0, g_hook_calls);
}
