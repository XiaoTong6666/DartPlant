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
  });
}
