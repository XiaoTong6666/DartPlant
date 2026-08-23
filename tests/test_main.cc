// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <iostream>

#include "test_runner.h"

namespace dartplant::testing {

std::vector<TestCase>& Tests() {
    static std::vector<TestCase> tests;
    return tests;
}

}  // namespace dartplant::testing

int main() {
    int failed = 0;
    for (const auto& test : dartplant::testing::Tests()) {
        try {
            test.body();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << dartplant::testing::Tests().size() - failed << " passed, " << failed
              << " failed\n";
    return failed == 0 ? 0 : 1;
}
