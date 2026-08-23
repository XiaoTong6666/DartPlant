// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#ifndef DARTPLANT_TESTS_TEST_RUNNER_H_
#define DARTPLANT_TESTS_TEST_RUNNER_H_

#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dartplant::testing {

struct TestCase {
    const char* name;
    void (*body)();
};

std::vector<TestCase>& Tests();

class Register final {
public:
    Register(const char* name, void (*body)()) { Tests().push_back({name, body}); }
};

[[noreturn]] inline void Fail(const char* file, int line, const std::string& message) {
    std::ostringstream stream;
    stream << file << ':' << line << ": " << message;
    throw std::runtime_error(stream.str());
}

}  // namespace dartplant::testing

#define TEST_CASE(name)                                                                            \
    static void name();                                                                            \
    static ::dartplant::testing::Register register_##name(#name, &name);                           \
    static void name()

#define EXPECT_TRUE(value)                                                                         \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            ::dartplant::testing::Fail(__FILE__, __LINE__, "EXPECT_TRUE(" #value ") failed");      \
        }                                                                                          \
    } while (false)

#define EXPECT_FALSE(value) EXPECT_TRUE(!(value))

#define EXPECT_EQ(expected, actual)                                                                \
    do {                                                                                           \
        const auto expected_value = (expected);                                                    \
        const auto actual_value = (actual);                                                        \
        if (!(expected_value == actual_value)) {                                                   \
            std::ostringstream message;                                                            \
            message << "EXPECT_EQ failed: expected " << expected_value << ", actual "              \
                    << actual_value;                                                               \
            ::dartplant::testing::Fail(__FILE__, __LINE__, message.str());                         \
        }                                                                                          \
    } while (false)

#endif  // DARTPLANT_TESTS_TEST_RUNNER_H_
