import 'package:dartplant_fixture/main.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('fixture methods have stable baseline behavior', () {
    expect(fixture.addInt(7, 5), 12);
    expect(fixture.negateBool(true), isFalse);
    expect(fixture.echoObject(const FixtureObject(9)).toString(),
        'FixtureObject(9)');
    expect(fixture.addDouble(1.25, 2.5), 3.75);
  });

  testWidgets('all fixture calls are reachable from the UI', (tester) async {
    await tester.pumpWidget(const DartPlantFixtureApp());

    await tester.tap(find.byKey(const ValueKey('fixture-int')));
    await tester.pump();
    expect(find.text('int:12'), findsOneWidget);

    await tester.tap(find.byKey(const ValueKey('fixture-bool')));
    await tester.pump();
    expect(find.text('bool:false'), findsOneWidget);

    await tester.tap(find.byKey(const ValueKey('fixture-object')));
    await tester.pump();
    expect(find.text('object:FixtureObject(9)'), findsOneWidget);
    await tester.pump(const Duration(milliseconds: 700));
    expect(find.text('object:FixtureObject(10)'), findsOneWidget);

    await tester.tap(find.byKey(const ValueKey('fixture-double')));
    await tester.pump();
    expect(find.text('double:3.75'), findsOneWidget);
  });
}
