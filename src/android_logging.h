// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#pragma once

#if defined(__ANDROID__)
#include <android/log.h>

#include <cstdarg>
#include <string>

namespace dartplant {

inline constexpr char kAndroidLogTag[] = "DartPlant";

inline void AndroidLogVPrint(int priority, const char* subsystem, const char* format,
                             va_list args) {
    std::string prefixed_format = "[";
    prefixed_format += subsystem;
    prefixed_format += "] ";
    prefixed_format += format;
    __android_log_vprint(priority, kAndroidLogTag, prefixed_format.c_str(), args);
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
