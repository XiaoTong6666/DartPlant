// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

extern "C" __attribute__((visibility("default"), noinline)) int DartPlantFixtureAdd(int left,
                                                                                    int right) {
    return left + right;
}

extern "C" __attribute__((visibility("default"), noinline)) bool DartPlantFixtureBool() {
    return true;
}
