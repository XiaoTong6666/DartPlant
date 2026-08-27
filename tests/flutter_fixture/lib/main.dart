import 'package:flutter/material.dart';
import 'dart:io';
import 'dart:typed_data';

import 'dartplant_native.dart';

const fixture = DartPlantFixture();

@pragma('vm:entry-point')
@pragma('vm:never-inline')
int instrumentedAdd(int left, int right) {
  final result = left + right;
  return result;
}

@pragma('vm:entry-point')
@pragma('vm:never-inline')
Object? nullableEchoObject(Object? value) {
  if (value is FixtureObject && value.value == -1) return null;
  return value;
}

@pragma('vm:entry-point')
@pragma('vm:never-inline')
bool negateBool(bool value) => !value;

@pragma('vm:entry-point')
@pragma('vm:never-inline')
T signatureProbe<T>(T value, {required bool enabled, int count = 0}) => value;

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final initializeStartStatus = DartPlantNative.startInitialize();
  runApp(const DartPlantFixtureApp());

  // Do not stall the UI isolate before runApp while the live-VM sampler is
  // looking for a Dart mutator context. A real Flutter application
  // keeps executing Dart during startup, so the fixture should exercise the
  // bootstrap under the same workload instead of an artificial await-only
  // event loop.
  WidgetsBinding.instance.addPostFrameCallback((_) async {
    final initializeStatus = initializeStartStatus == 0
        ? await DartPlantNative.waitForInitialization()
        : initializeStartStatus;
    debugPrint('DartPlant initialize status: $initializeStatus');
    DartPlantNative.resetNullSemanticProbe();
    final canonicalNull = nullableEchoObject(null);
    final rewrittenToNull = nullableEchoObject(FixtureObject(11));
    final nullProbe = DartPlantNative.nullSemanticProbe();
    debugPrint(
      'DartPlant null semantic probe: $nullProbe values=$canonicalNull/$rewrittenToNull',
    );
    DartPlantNative.resetBoolSemanticProbe();
    final boolTrue = negateBool(false);
    final boolFalse = negateBool(true);
    final boolProbe = DartPlantNative.boolSemanticProbe();
    debugPrint(
      'DartPlant bool semantic probe: $boolProbe values=$boolTrue/$boolFalse',
    );
    DartPlantNative.resetInstrumentedAddProbe();
    for (var index = 0; index < 5; ++index) {
      instrumentedAdd(2, 3);
    }
    final startupProbe = DartPlantNative.instrumentedAddProbe();
    debugPrint('DartPlant live VM startup probe: $startupProbe');
  });
}

final class FixtureObject {
  const FixtureObject(this.value);

  final int value;

  @override
  String toString() => 'FixtureObject($value)';
}

final class DartPlantFixture {
  const DartPlantFixture();

  @pragma('vm:entry-point')
  @pragma('vm:never-inline')
  int addInt(int left, int right) => left + right;

  @pragma('vm:entry-point')
  @pragma('vm:never-inline')
  bool negateBool(bool value) => !value;

  @pragma('vm:entry-point')
  @pragma('vm:never-inline')
  Object? echoObject(Object? value) => value;

  @pragma('vm:entry-point')
  @pragma('vm:never-inline')
  double addDouble(double left, double right) => left + right;
}

final class DartPlantFixtureApp extends StatelessWidget {
  const DartPlantFixtureApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'DartPlant AOT Fixture',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xff006c4c)),
        useMaterial3: true,
      ),
      home: const FixtureScreen(),
    );
  }
}

final class FixtureScreen extends StatefulWidget {
  const FixtureScreen({super.key});

  @override
  State<FixtureScreen> createState() => _FixtureScreenState();
}

final class _FixtureScreenState extends State<FixtureScreen> {
  String _result = 'ready';
  int _left = 7;
  int _right = 5;
  bool _flag = true;
  double _doubleLeft = 1.25;
  double _doubleRight = 2.5;

  void _show(String result) => setState(() => _result = result);

  void _runInstrumentedAdd() {
    DartPlantNative.resetInstrumentedAddProbe();
    const calls = 5;
    var value = 0;
    for (var index = 0; index < calls; ++index) {
      value = instrumentedAdd(2, 3);
    }
    final native = DartPlantNative.instrumentedAddProbe();
    _show('instrumented:$value calls=$calls native=$native');
  }

  Future<void> _runObjectProbe() async {
    if (Platform.isAndroid) DartPlantNative.beginObjectProbe();
    final first = 'object:${fixture.echoObject(FixtureObject(9))}';
    _show(first);
    // The native callback has returned, so this root can no longer protect
    // the object while the allocation-pressure phase runs. Host widget tests
    // do not load the Android-only fixture bridge.
    if (Platform.isAndroid) DartPlantNative.releaseObjectRoot();
    await Future<void>.delayed(const Duration(milliseconds: 300));
    final pressure = <Uint8List>[];
    for (var index = 0; index < 6000; ++index) {
      pressure.add(Uint8List(8192));
    }
    pressure.clear();
    await Future<void>.delayed(const Duration(milliseconds: 300));
    final second = fixture.echoObject(FixtureObject(10));
    _show('object:$second');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('DartPlant AOT Fixture')),
      body: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            SelectableText(
              _result,
              key: const ValueKey('fixture-result'),
              style: Theme.of(context).textTheme.headlineSmall,
            ),
            const SizedBox(height: 24),
            FilledButton(
              key: const ValueKey('fixture-instrumented-add'),
              onPressed: _runInstrumentedAdd,
              child: const Text('instrumentedAdd'),
            ),
            FilledButton(
              key: const ValueKey('fixture-int'),
              onPressed: () {
                final value = fixture.addInt(_left, _right);
                _left += 2;
                _show('int:$value');
              },
              child: const Text('int'),
            ),
            FilledButton(
              key: const ValueKey('fixture-bool'),
              onPressed: () {
                final value = fixture.negateBool(_flag);
                _flag = !_flag;
                _show('bool:$value');
              },
              child: const Text('bool'),
            ),
            FilledButton(
              key: const ValueKey('fixture-object'),
              onPressed: _runObjectProbe,
              child: const Text('Object?'),
            ),
            FilledButton(
              key: const ValueKey('fixture-double'),
              onPressed: () {
                final value = fixture.addDouble(_doubleLeft, _doubleRight);
                _doubleLeft += 0.25;
                _doubleRight += 0.5;
                _show('double:$value');
              },
              child: const Text('double'),
            ),
          ],
        ),
      ),
    );
  }
}
