// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <cstring>

#include "runtime/runtime_internal.h"

namespace dartplant {
namespace {

std::string CopyString(const char* value) {
    return value == nullptr ? std::string() : std::string(value);
}

}  // namespace

void RuntimeProfileStorage::Assign(const DartPlantRuntimeProfile& source) {
    profile = source;
    profile_name = CopyString(source.profile_name);
    dart_version = CopyString(source.dart_version);
    flutter_version = CopyString(source.flutter_version);
    app_module_name = CopyString(source.app_module_name);
    app_build_id = CopyString(source.app_build_id);
    runtime_module_name = CopyString(source.runtime_module_name);
    runtime_build_id = CopyString(source.runtime_build_id);
    profile.profile_name = profile_name.c_str();
    profile.dart_version = dart_version.c_str();
    profile.flutter_version = flutter_version.c_str();
    profile.app_module_name = app_module_name.c_str();
    profile.app_build_id = app_build_id.c_str();
    profile.runtime_module_name = runtime_module_name.c_str();
    profile.runtime_build_id = runtime_build_id.c_str();
}

}  // namespace dartplant

extern "C" void dartplant_runtime_profile_init_arm64_aot(DartPlantRuntimeProfile* profile) {
    if (profile == nullptr) return;
    std::memset(profile, 0, sizeof(*profile));
    profile->struct_size = sizeof(*profile);
    profile->profile_version = 1;
    profile->runtime_kind = DARTPLANT_RUNTIME_FLUTTER_AOT;
    profile->architecture = DARTPLANT_ARCH_ARM64;
    profile->pointer_size = 8;
    profile->profile_name = "flutter-aot-arm64-conservative";
    profile->app_module_name = "libapp.so";
    profile->runtime_module_name = "libflutter.so";
    profile->result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};
    profile->thread_gp_register = 0xff;
    profile->pool_gp_register = 0xff;
    for (auto& location : profile->argument_locations) {
        location = {DARTPLANT_ABI_GP_REGISTER, 0xff, {0, 0}};
    }
}
