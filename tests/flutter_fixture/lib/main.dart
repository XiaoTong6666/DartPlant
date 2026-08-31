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

// Keep this as an ordinary direct-call-only optimized AOT body. Without a
// tear-off the compiler is free to use the unboxed double Dart calling
// convention; the native fixture proves V0/V1 argument access from evidence.
@pragma('vm:never-inline')
double verifiedAbiDouble(double left, double right) {
  return (left * 1.5) + right + 0.25;
}

// P6 compiler-produced ABI corpus. Keep these ordinary direct calls free of
// tear-offs so vm.unboxing-info.metadata is the source of truth for the
// optimized PRODUCT calling convention.
@pragma('vm:never-inline')
int verifiedAbiInt64(int left, int right) {
  return (left * 10000000000) + right;
}

@pragma('vm:never-inline')
double verifiedAbiEntryStack(
  double a0,
  double a1,
  double a2,
  double a3,
  double a4,
  double a5,
  double a6,
  double a7,
) {
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + (a7 * 10.0);
}

// Seven unboxed doubles consume V0-V5 plus one Dart stack slot. The exact x15
// parity belongs to the caller frame layout and is intentionally not part of
// this ABI assertion.
@pragma('vm:never-inline')
double verifiedAbiOddStack(
  double a0,
  double a1,
  double a2,
  double a3,
  double a4,
  double a5,
  double a6,
) {
  return a0 + a1 + a2 + a3 + a4 + a5 + (a6 * 10.0);
}

// vm:entry-point makes this callable from native code, which forces the
// compiler's boxed stack calling convention instead of register-CC.
@pragma('vm:entry-point')
@pragma('vm:never-inline')
int verifiedAbiForcedStack(int left, int right) {
  return (left * 10) + right;
}

@pragma('vm:never-inline')
@pragma('vm:entry-point')
int invokeForcedStackClosure(
  int Function(int, int) callback,
  int left,
  int right,
) =>
    callback(left, right);

@pragma('vm:never-inline')
(Object?, Object?) verifiedAbiPair(Object? left, Object? right) {
  return (left, right);
}

@pragma('vm:never-inline')
double verifiedAbiThrowingStack(
  double a0,
  double a1,
  double a2,
  double a3,
  double a4,
  double a5,
  double a6,
  double a7,
) {
  if (a0 == 99.0) {
    throw StateError('dartplant-p6-throw');
  }
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + (a7 * 10.0);
}

@pragma('vm:never-inline')
int verifiedAbiImmediateCatchProbe() {
  try {
    verifiedAbiThrowingStack(99, 2, 3, 4, 5, 6, 7, 8);
    return 1;
  } catch (_) {
    return 2;
  }
}

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

    // The advanced runtime is intentionally initialized with DartPlant's local
    // publication-gate policy. Exercise one real Dart call before the simple
    // facade/P6 consumers replace the process-default host with the strict
    // Dobby adapter. This first call therefore proves both the generated entry
    // gate and the process-global JumpToFrame bridge on the legacy-host path.
    final localGateWarmup = instrumentedAdd(2, 3);
    final localGateWarmupPassed =
        initializeStatus == 0 && localGateWarmup == 115;
    debugPrint(
      'DartPlant local gate real-Dart warmup: ${localGateWarmupPassed ? 1 : 0} value=$localGateWarmup',
    );

    // The first two calls are owned only by the simple-facade consumer TU.
    // It lazy-bootstraps its own default runtime, consumes the embedded
    // compiler artifact, derives the typed V0/V1 -> V0 layout, and installs two
    // logical HookHandles without including any advanced DartPlant header.
    final simpleFacadeInstall = DartPlantNative.simpleFacadeInstall();
    debugPrint('DartPlant simple facade install: $simpleFacadeInstall');
    final simpleFacadeFirst = verifiedAbiDouble(1.25, 2.5);
    final simpleFacadeStage1 = DartPlantNative.simpleFacadeStage1();
    final simpleFacadeSecond = verifiedAbiDouble(2.0, 3.0);
    final simpleFacadeStage2 = DartPlantNative.simpleFacadeStage2();
    final simpleFacadePassed = simpleFacadeInstall == 0 &&
        simpleFacadeFirst == 27.625 &&
        simpleFacadeSecond == 29.25 &&
        simpleFacadeStage1 == 1 &&
        simpleFacadeStage2 == 1;
    debugPrint(
      'DartPlant simple facade typed hook: ${simpleFacadePassed ? 1 : 0} values=$simpleFacadeFirst/$simpleFacadeSecond stages=$simpleFacadeStage1/$simpleFacadeStage2',
    );

    final p6BaselineInt64 = verifiedAbiInt64(100000000, 7);
    final p6BaselineStack = verifiedAbiEntryStack(1, 2, 3, 4, 5, 6, 7, 8);
    final p6BaselineOdd = verifiedAbiOddStack(1, 2, 3, 4, 5, 6, 7);
    final p6BaselineThrow = verifiedAbiThrowingStack(1, 2, 3, 4, 5, 6, 7, 8);
    final p6BaselineForced = verifiedAbiForcedStack(3, 4);
    final p6BaselinePair = verifiedAbiPair(21, 22);
    final p6Install = DartPlantNative.p6AbiInstall();
    final p6HookedInt64 = verifiedAbiInt64(300000000, 13);
    final p6HookedStack = verifiedAbiEntryStack(2, 3, 4, 5, 6, 7, 8, 9);
    final p6HookedOdd = verifiedAbiOddStack(2, 3, 4, 5, 6, 7, 8);
    var p6ThrowPath = 0;
    try {
      p6ThrowPath = verifiedAbiImmediateCatchProbe();
    } catch (_) {
      p6ThrowPath = 3;
    }
    final p6HookedThrow = verifiedAbiThrowingStack(2, 3, 4, 5, 6, 7, 8, 9);
    debugPrint(
        'DartPlant P6 throw path: $p6ThrowPath normal=$p6BaselineThrow/$p6HookedThrow');
    final p6HookedForced = verifiedAbiForcedStack(5, 6);
    final p6HookedPair = verifiedAbiPair(31, 32);
    final p6Probe = DartPlantNative.p6AbiProbe();
    final p6Passed = p6Install == 0 &&
        p6Probe == 1 &&
        p6BaselineInt64 == 1000000000000000007 &&
        p6HookedInt64 == 3000000010000000113 &&
        p6BaselineStack == 108.0 &&
        p6HookedStack == 1146.0 &&
        p6BaselineOdd == 91.0 &&
        p6HookedOdd == 217.0 &&
        p6ThrowPath == 2 &&
        p6BaselineThrow == 108.0 &&
        p6HookedThrow == 125.0 &&
        p6BaselineForced == 34 &&
        p6HookedForced == 65 &&
        p6BaselinePair.$1 == 21 &&
        p6BaselinePair.$2 == 22 &&
        p6HookedPair.$1 == 32 &&
        p6HookedPair.$2 == 31;
    debugPrint(
      'DartPlant P6 ABI corpus: ${p6Passed ? 1 : 0} install=$p6Install probe=$p6Probe int64=$p6BaselineInt64/$p6HookedInt64 stack=$p6BaselineStack/$p6HookedStack odd=$p6BaselineOdd/$p6HookedOdd forced=$p6BaselineForced/$p6HookedForced pair=${p6BaselinePair.$1},${p6BaselinePair.$2}/${p6HookedPair.$1},${p6HookedPair.$2}',
    );

    // Run the exception-bridge lifetime race with exactly one real-Dart hook
    // consumer. Its enter callback requests unhook while in flight, then the
    // Dart body throws. The immediate caller must still catch it and the
    // process-global JumpToFrame backup must remain valid through cleanup.
    final exceptionLifetimeInstall =
        DartPlantNative.exceptionBridgeLifetimeInstall();
    var exceptionLifetimeCatch = 0;
    try {
      exceptionLifetimeCatch = verifiedAbiImmediateCatchProbe();
    } catch (_) {
      exceptionLifetimeCatch = 3;
    }
    final exceptionLifetimeProbe =
        DartPlantNative.exceptionBridgeLifetimeProbe();
    final exceptionLifetimePassed = exceptionLifetimeInstall == 0 &&
        exceptionLifetimeCatch == 2 &&
        exceptionLifetimeProbe == 1;
    debugPrint(
      'DartPlant exception bridge lifetime: ${exceptionLifetimePassed ? 1 : 0} install=$exceptionLifetimeInstall catch=$exceptionLifetimeCatch probe=$exceptionLifetimeProbe',
    );

    // Every independent artifact consumer above has now removed its physical
    // hooks and shut down. The advanced runtime already prebound the pristine
    // artifact registry during bootstrap, so it is safe to patch the exact
    // AOT-dropped implicit-closure target without invalidating another
    // runtime's whole-bundle fingerprint validation.
    final forcedStackClosureInstall =
        DartPlantNative.enableForcedStackClosureHook();
    const forcedStackTearOff = verifiedAbiForcedStack;
    final forcedStackClosureValue =
        invokeForcedStackClosure(forcedStackTearOff, 7, 8);
    final forcedStackClosureProbe = DartPlantNative.forcedStackClosureProbe();
    final forcedStackClosurePassed = forcedStackClosureInstall == 0 &&
        forcedStackClosureValue == 78 &&
        forcedStackClosureProbe == 1;
    debugPrint(
      'DartPlant closure receiver probe: ${forcedStackClosurePassed ? 1 : 0} value=$forcedStackClosureValue native=$forcedStackClosureProbe install=$forcedStackClosureInstall',
    );

    // Only after the simple consumer has removed its final subscription and
    // the P6/exception consumers have removed all artifact-first hooks and
    // shut down may the advanced fixture reuse physical entry targets for its
    // ABI/late-shared diagnostics.
    final advancedOrdinaryHook = DartPlantNative.enableAdvancedOrdinaryHook();
    debugPrint(
        'DartPlant advanced ordinary hook enable: $advancedOrdinaryHook');

    DartPlantNative.resetNullSemanticProbe();
    final canonicalNull = nullableEchoObject(null);
    final rewrittenToNull = nullableEchoObject(const FixtureObject(11));
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
    DartPlantNative.resetVerifiedAbiDoubleProbe();
    final ordinaryDirect = verifiedAbiDouble(1.25, 2.5);
    final lateSharedTransition = DartPlantNative.markVerifiedAbiDoubleShared();
    final ordinaryAfterShared = verifiedAbiDouble(2.0, 3.0);
    final ordinaryProbe = DartPlantNative.verifiedAbiDoubleProbe();
    debugPrint(
      'DartPlant ordinary AOT calls: direct=$ordinaryDirect afterShared=$ordinaryAfterShared',
    );
    debugPrint(
      'DartPlant ordinary AOT typed probe: $ordinaryProbe values=$ordinaryDirect/$ordinaryAfterShared',
    );
    final lateSharedPassed = lateSharedTransition == 1 &&
        ordinaryProbe == 1 &&
        ordinaryDirect == 16.125 &&
        ordinaryAfterShared == 6.25;
    debugPrint(
      'DartPlant late shared typed fail-close: ${lateSharedPassed ? 1 : 0} transition=$lateSharedTransition values=$ordinaryDirect/$ordinaryAfterShared',
    );
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
  final int _right = 5;
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
    final first = 'object:${fixture.echoObject(const FixtureObject(9))}';
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
    final second = fixture.echoObject(const FixtureObject(10));
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
