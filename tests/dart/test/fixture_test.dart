// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

import 'package:dartplant_fixture/fixture.dart';
import 'package:test/test.dart';

void main() {
  group('DartPlantFixture', () {
    const fixture = DartPlantFixture();

    test('keeps arithmetic behavior stable', () {
      expect(fixture.add(2, 3), 5);
      expect(fixture.returnSmallInteger(), 42);
    });

    test('keeps tagged value behavior stable', () {
      expect(fixture.negate(true), isFalse);
      expect(fixture.echo(null), isNull);
      expect(fixture.echo('dartplant'), 'dartplant');
      expect(topLevelBoolean(), isTrue);
    });

    test('keeps mixed GP/FPU ABI fixture behavior stable', () {
      expect(fixture.mixedRegisterAbi(1.5, 4, 2.25, null), 7.75);
      expect(fixture.mixedRegisterAbi(1.5, 4, 2.25, const Object()), 8.75);
    });

    test('keeps entry-stack and branch fixtures stable', () {
      expect(fixture.sevenDoubleArguments(1, 2, 3, 4, 5, 6, 7), 28);
      expect(fixture.branchMerge(10, 20, true), 11);
      expect(fixture.branchMerge(10, 20, false), 21);
    });

    test('keeps closure and implicit tear-off semantics stable', () {
      final addTen = fixture.makeAdder(10);
      expect(addTen(7), 17);

      final tearOff = fixture.addTearOff();
      expect(tearOff(4, 9), 13);
    });
  });
}
