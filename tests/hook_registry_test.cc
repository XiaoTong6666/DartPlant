// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <dlfcn.h>

#include <type_traits>

#include "core/internal.h"
#include "dartplant/native_api.h"
#include "test_runner.h"

namespace {

static_assert(std::is_same_v<DartPlantHostHook, DartPlantNativeHook>);
static_assert(std::is_same_v<DartPlantHostUnhook, DartPlantNativeUnhook>);

int g_hook_calls = 0;
int g_unhook_calls = 0;
void* g_last_target = nullptr;

struct ContextHostState {
    int hook_calls = 0;
    int unhook_calls = 0;
    void* last_target = nullptr;
};

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

int FakeHostHook(void*, void* target, void* replacement, void** backup) {
    return FakeHook(target, replacement, backup);
}

int FakeHostUnhook(void*, void* target) { return FakeUnhook(target); }

int ContextHook(void* user_data, void* target, void*, void** backup) {
    auto* state = static_cast<ContextHostState*>(user_data);
    if (state == nullptr || backup == nullptr) return -1;
    ++state->hook_calls;
    state->last_target = target;
    *backup = target;
    return 0;
}

int ContextUnhook(void* user_data, void* target) {
    auto* state = static_cast<ContextHostState*>(user_data);
    if (state == nullptr) return -1;
    ++state->unhook_calls;
    state->last_target = target;
    return 0;
}

int Replacement(int, int) { return 42; }

void ResetFakeHost() {
    dartplant_reset();
    g_hook_calls = 0;
    g_unhook_calls = 0;
    g_last_target = nullptr;
    const DartPlantHostApi api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = nullptr,
        .hook = FakeHostHook,
        .unhook = FakeHostUnhook,
    };
    dartplant::InstallHostApi(&api);
    dartplant::RefreshModules();
}

}  // namespace

TEST_CASE(GenericHostApiRetainsBackendInstanceForInstalledHook) {
    dartplant_reset();
    ContextHostState first;
    ContextHostState second;
    const DartPlantHostApi first_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &first,
        .hook = ContextHook,
        .unhook = ContextUnhook,
    };
    const DartPlantHostApi second_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &second,
        .hook = ContextHook,
        .unhook = ContextUnhook,
    };
    EXPECT_EQ(DARTPLANT_OK, dartplant_install_host_api(&first_api));

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
    EXPECT_EQ(1, first.hook_calls);
    EXPECT_EQ(0, second.hook_calls);

    // Replacing the process default backend must not change ownership of a
    // physical hook already installed by the first backend instance.
    EXPECT_EQ(DARTPLANT_OK, dartplant_install_host_api(&second_api));
    EXPECT_EQ(DARTPLANT_OK, dartplant_unhook(hook));
    EXPECT_EQ(1, first.unhook_calls);
    EXPECT_EQ(0, second.unhook_calls);
    dartplant_release_hook(hook);
    dlclose(fixture);
}

TEST_CASE(SimpleInitOwnsRuntimeProfileAndBootstrapsFindLazily) {
    dartplant_reset();
    ContextHostState host_state;
    const DartPlantHostApi host_api = {
        .struct_size = sizeof(DartPlantHostApi),
        .version = DARTPLANT_HOST_API_VERSION,
        .user_data = &host_state,
        .hook = ContextHook,
        .unhook = ContextUnhook,
    };
    const DartPlantInitInfo init = {
        .struct_size = sizeof(DartPlantInitInfo),
        .version = DARTPLANT_INIT_API_VERSION,
        .host_api = &host_api,
        .artifact_bundle = nullptr,
        .app_module_name = nullptr,
        .runtime_module_name = nullptr,
    };
    EXPECT_EQ(DARTPLANT_OK, dartplant_init(&init));
    EXPECT_EQ(1, static_cast<int>(dartplant_is_initialized()));

    // No Flutter images exist in the host test process. The important public
    // contract is that find_method owns refresh/bootstrap and fails at runtime
    // readiness rather than asking the caller for LiveVmContext/Profile data.
    const DartPlantMethodQuery query = {
        .struct_size = sizeof(query),
        .library_uri = "package:fixture/main.dart",
        .class_name = "Global",
        .function_name = "missing",
        .signature = "",
        .entry_kind = DARTPLANT_ENTRY_DEFAULT,
    };
    DartPlantMethod* method = nullptr;
    EXPECT_EQ(DARTPLANT_RUNTIME_NOT_READY, dartplant_find_method(&query, &method));
    EXPECT_TRUE(method == nullptr);

    dartplant_shutdown();
    EXPECT_EQ(0, static_cast<int>(dartplant_is_initialized()));
}

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
