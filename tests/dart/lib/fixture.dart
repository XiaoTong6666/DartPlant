// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

/// Stable Dart functions used by the offline indexer and ARM64 AOT tests.
///
/// Keep these functions simple and avoid changing their signatures without
/// updating the corresponding metadata fixtures.
final class DartPlantFixture {
  const DartPlantFixture();

  @pragma('vm:never-inline')
  int add(int left, int right) => left + right;

  @pragma('vm:never-inline')
  bool negate(bool value) => !value;

  @pragma('vm:never-inline')
  int returnSmallInteger() => 42;

  @pragma('vm:never-inline')
  Object? echo(Object? value) => value;
}

@pragma('vm:never-inline')
bool topLevelBoolean() => true;
