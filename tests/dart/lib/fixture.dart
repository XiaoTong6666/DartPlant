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

  /// Keeps GP and FPU parameters interleaved so compiler/runtime tests can
  /// verify that Dart ARM64 allocates the two register files independently.
  @pragma('vm:never-inline')
  double mixedRegisterAbi(double first, int count, double second, Object? tag) {
    final taggedBias = tag == null ? 0 : 1;
    return first + second + count + taggedBias;
  }

  /// Seven doubles exceed Dart ARM64's six direct FPU argument registers and
  /// force the final value through the Dart x15-relative entry stack.
  @pragma('vm:never-inline')
  double sevenDoubleArguments(
    double a,
    double b,
    double c,
    double d,
    double e,
    double f,
    double g,
  ) =>
      a + b + c + d + e + f + g;

  /// Gives the structural analyzer a small real Dart branch merge while
  /// preserving a simple, deterministic semantic oracle in `dart test`.
  @pragma('vm:never-inline')
  int branchMerge(int left, int right, bool chooseLeft) {
    final selected = chooseLeft ? left : right;
    return selected + 1;
  }

  /// Keeps both a user closure and an implicit instance-method tear-off in the
  /// AOT corpus. DartPlant treats the Closure object carried in ARM64 x0 as a
  /// hidden receiver rather than a FunctionType formal.
  @pragma('vm:never-inline')
  int Function(int) makeAdder(int bias) {
    @pragma('vm:never-inline')
    int addBias(int value) => bias + value;
    return addBias;
  }

  @pragma('vm:never-inline')
  int Function(int, int) addTearOff() => add;
}

@pragma('vm:never-inline')
bool topLevelBoolean() => true;
