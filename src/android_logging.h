// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#pragma once

#if defined(__ANDROID__)
#include <android/log.h>

#include <array>
#include <cstdarg>
#include <cstdio>

namespace dartplant {

inline constexpr char kAndroidLogTag[] = "DartPlant";

inline void AndroidLogVPrint(int priority, const char* subsystem, const char* format,
                             va_list args) {
    std::array<char, 1024> message{};
    std::vsnprintf(message.data(), message.size(), format, args);
    __android_log_print(priority, kAndroidLogTag, "[%s] %s",
                        subsystem == nullptr ? "Core" : subsystem, message.data());
}

inline void AndroidLogPrint(int priority, const char* subsystem, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

inline void AndroidLogPrint(int priority, const char* subsystem, const char* format, ...) {
    va_list args;
    va_start(args, format);
    AndroidLogVPrint(priority, subsystem, format, args);
    va_end(args);
}

}  // namespace dartplant
#endif
