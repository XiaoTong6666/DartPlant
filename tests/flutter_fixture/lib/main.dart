import 'package:flutter/material.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'dartplant_native.dart';
import 'deferred_probe.dart' deferred as deferred_probe;
import 'package:flutter/services.dart';

const fixture = DartPlantFixture();
const _launchChannel = MethodChannel('dev.dartplant.fixture/launch');
const _secondaryChannel = MethodChannel('dev.dartplant.fixture/secondary');
const _ciFlutterVersion = String.fromEnvironment(
  'DARTPLANT_CI_FLUTTER_VERSION',
  defaultValue: 'unknown',
);
const _ciDartVersion = String.fromEnvironment(
  'DARTPLANT_CI_DART_VERSION',
  defaultValue: 'unknown',
);
const _ciTargetAbi = String.fromEnvironment(
  'DARTPLANT_CI_TARGET_ABI',
  defaultValue: 'unknown',
);
const _ciRuntimeTests = <String>{
  'normal',
  'arguments_descriptor',
  'closure',
  'generic_closure',
  'generic_gc',
  'exception',
  'transition',
  'artifact_revalidate',
  'deferred_lifecycle',
  'multi_engine',
};
const _ciCommonScenarios = <String>{
  'initialization',
  'local_gate',
  'simple_facade',
  'p6_abi',
  'exception_bridge',
  'closure_receiver',
  'advanced_ordinary',
  'null_semantics',
  'bool_semantics',
  'live_vm_startup',
  'ordinary_aot',
  'late_shared',
};
final _ciScenarioResults = <String, bool>{};

void _ciEvent(String event, Map<String, Object?> fields) {
  final payload = <String, Object?>{'event': event, ...fields};
  debugPrint('DARTPLANT_CI ${jsonEncode(payload)}', wrapWidth: 4096);
}

void _ciScenario(String name, bool passed,
    [Map<String, Object?> fields = const {}]) {
  _ciScenarioResults[name] = passed;
  _ciEvent('scenario', <String, Object?>{
    'name': name,
    'state': passed ? 'pass' : 'fail',
    ...fields,
  });
}

void movingGcPressure(SendPort port) {
  port.send(1);
  final retained = <List<Object?>>[];
  for (var index = 0; index < 200000; ++index) {
    retained.add(List<Object?>.filled(64, Object()));
    if (retained.length == 128) retained.clear();
  }
  port.send(2);
}

Future<void> _multiOwnerGcPressure() async {
  // Keep enough survivors around for promotion while repeatedly churning
  // young-space. Non-product Dart 3.12.x is expected to compact old space
  // opportunistically; the native re-bootstrap afterwards must tolerate every
  // heap-root relocation without changing the IsolateGroup incarnation.
  final retained = <List<Object?>>[];
  for (var round = 0; round < 24; ++round) {
    for (var index = 0; index < 4096; ++index) {
      retained.add(
          List<Object?>.filled(64, _GcPressureMarker((round << 12) | index)));
      if (retained.length > 512) retained.removeRange(0, 256);
    }
    await Future<void>.delayed(Duration.zero);
  }
  retained.clear();
}

@pragma('vm:entry-point')
Future<void> secondaryEngineMain() async {
  WidgetsFlutterBinding.ensureInitialized();
  _bootstrapRetainedGenericClosure = retainedGenericClosure;
  _secondaryChannel.setMethodCallHandler((call) async {
    final arguments = call.arguments;
    final label = arguments is Map && arguments['label'] is int
        ? arguments['label'] as int
        : 2;
    switch (call.method) {
      case 'activate':
        return <String, Object?>{
          'epoch': DartPlantNative.multiOwnerActivate(label),
        };
      case 'hookTarget':
        return instrumentedAdd(2, 3);
      case 'gc':
        final before = DartPlantNative.multiOwnerActivate(label);
        await _multiOwnerGcPressure();
        final after = DartPlantNative.multiOwnerActivate(label);
        return <String, Object?>{
          'before': before,
          'after': after,
        };
      case 'deferred':
        final before = DartPlantNative.multiOwnerActivate(label);
        await deferred_probe.loadLibrary();
        final after = DartPlantNative.multiOwnerDeferred(label);
        final value = deferred_probe.deferredAdd(1);
        return <String, Object?>{
          'before': before,
          'after': after,
          'value': value,
        };
      default:
        throw PlatformException(
          code: 'unknown-secondary-command',
          message: 'Unknown secondary-engine command: ${call.method}',
        );
    }
  });
  await _secondaryChannel.invokeMethod<void>('ready');
}

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

typedef SignatureProbeClosure = T Function<T>(
  T value, {
  required bool enabled,
  int count,
});

// Keep a real generic tear-off alive in PRODUCT. The proof calls it through a
// generic function-typed parameter so AOT cannot replace the invocation with a
// direct call to signatureProbe; the closure calling convention must carry x4
// ArgumentsDescriptor and the explicit TypeArguments vector.
const SignatureProbeClosure retainedGenericClosure = signatureProbe;
SignatureProbeClosure? _bootstrapRetainedGenericClosure;

final class TypeArgumentsProofValue {
  const TypeArgumentsProofValue(this.value);

  final int value;

  @override
  String toString() => 'TypeArgumentsProofValue($value)';
}

@pragma('vm:entry-point')
@pragma('vm:never-inline')
List<U> invokeRetainedGenericClosure<U>(
  SignatureProbeClosure callback,
  U value,
) {
  final argument = <U>[value];
  return callback<List<U>>(
    argument,
    enabled: true,
    count: 1,
  );
}

@pragma('vm:entry-point')
@pragma('vm:never-inline')
int typeArgumentsGcPressure() {
  // This function is invoked from the native enter callback through Dart_Invoke.
  // Churn enough young-space allocation to force at least one scavenge while
  // DartPlant's generated-root lease is the only relocation-safe copy of the
  // TypeArguments element available to the callback.
  var checksum = 0;
  final retained = <List<Object?>>[];
  for (var round = 0; round < 12; ++round) {
    for (var index = 0; index < 4096; ++index) {
      final marker = _GcPressureMarker((round << 12) | index);
      retained.add(List<Object?>.filled(128, marker));
      checksum ^= marker.value + retained.last.length;
      if (retained.length == 192) retained.clear();
    }
  }
  return checksum & 0x7fffffff;
}

final class _GcPressureMarker {
  const _GcPressureMarker(this.value);

  final int value;
}

String _runTypeArgumentsProof(
  String source, {
  String scenario = 'generic_gc',
  bool requireRelocation = true,
}) {
  final callback = _bootstrapRetainedGenericClosure ?? retainedGenericClosure;
  final pressurePort = ReceivePort();
  try {
    final prepare = DartPlantNative.typeArgumentsProofPrepareMode(
      callback,
      pressurePort.sendPort,
      requireRelocation: requireRelocation,
    );
    debugPrint(
      'DartPlant app TypeArguments proof trigger: source=$source prepare=$prepare require_relocation=${requireRelocation ? 1 : 0} explicit=List<TypeArgumentsProofValue>',
    );
    if (prepare != 0) {
      final failed = 'typeargs:$source prepare=$prepare';
      debugPrint('DartPlant app TypeArguments proof: 0 $failed');
      _ciScenario(scenario, false, <String, Object?>{
        'source': source,
        'prepare': prepare,
        'require_relocation': requireRelocation,
      });
      return failed;
    }

    final value = invokeRetainedGenericClosure<TypeArgumentsProofValue>(
      callback,
      const TypeArgumentsProofValue(37),
    );
    final native = DartPlantNative.typeArgumentsProof();
    final resultOk = value.length == 1 &&
        value.single.value == 37 &&
        value.runtimeType.toString().contains('TypeArgumentsProofValue');
    final passed = native == 1 && resultOk;
    final summary =
        'typeargs:$source native=$native result=${value.single} runtimeType=${value.runtimeType}';
    debugPrint(
      'DartPlant app TypeArguments proof: ${passed ? 1 : 0} source=$source native=$native require_relocation=${requireRelocation ? 1 : 0} result_ok=${resultOk ? 1 : 0} value=${value.single} runtimeType=${value.runtimeType}',
    );
    _ciScenario(scenario, passed, <String, Object?>{
      'source': source,
      'native': native,
      'require_relocation': requireRelocation,
      'result_ok': resultOk,
    });
    return summary;
  } finally {
    pressurePort.close();
  }
}

int _mapInt(Map<Object?, Object?> value, String key) {
  final result = value[key];
  return result is int ? result : 0;
}

Future<Map<Object?, Object?>> _multiOwnerCommand(
  String command,
  int label,
) async {
  final result = await _launchChannel.invokeMethod<Object?>(
    'multiOwnerCommand',
    <String, Object?>{'command': command, 'label': label},
  );
  return result is Map ? Map<Object?, Object?>.from(result) : const {};
}

Future<bool> _runMultiEngineLifecycleProof() async {
  var aEpoch = 0;
  var aReturnEpoch = 0;
  var aPostDeferredEpoch = 0;
  var aPostDestroyEpoch = 0;
  var aFinalEpoch = 0;
  var bEpoch = 0;
  var b2Epoch = 0;
  var bGcBefore = 0;
  var bGcAfter = 0;
  var bDeferredBefore = 0;
  var bDeferredAfter = 0;
  var bDeferredValue = 0;
  var aHookBefore = 0;
  var aHookWhileBActive = 0;
  var aHookAfterB = 0;
  var aHookWhileBAfterDeferred = 0;
  var aHookAfterDeferred = 0;
  var aHookFinal = 0;
  var aHookWhileB2Active = 0;
  var bHookValue = -1;
  var b2HookValue = -1;
  var engineIncarnation1 = 0;
  var engineIncarnation2 = 0;
  String? failure;

  try {
    DartPlantNative.resetInstrumentedAddProbe();
    aEpoch = DartPlantNative.multiOwnerActivate(1);
    aHookBefore = instrumentedAdd(2, 3);

    final start = await _launchChannel.invokeMethod<Object?>('multiOwnerStart');
    if (start is Map) {
      engineIncarnation1 =
          start['incarnation'] is int ? start['incarnation'] as int : 0;
    }

    final bActivate = await _multiOwnerCommand('activate', 2);
    bEpoch = _mapInt(bActivate, 'epoch');
    final bHook = await _launchChannel.invokeMethod<Object?>(
      'multiOwnerCommand',
      <String, Object?>{'command': 'hookTarget', 'label': 2},
    );
    bHookValue = bHook is int ? bHook : -1;
    // Keep B as the runtime's active owner, then exercise the interactive
    // rebind path from A. A's method/listener belongs to another still-live
    // owner and must not be torn down merely because the active projection is B.
    DartPlantNative.resetInstrumentedAddProbe();
    aHookWhileBActive = instrumentedAdd(2, 3);

    aReturnEpoch = DartPlantNative.multiOwnerActivate(1);
    aHookAfterB = instrumentedAdd(2, 3);

    final bGc = await _multiOwnerCommand('gc', 2);
    bGcBefore = _mapInt(bGc, 'before');
    bGcAfter = _mapInt(bGc, 'after');

    final bDeferred = await _multiOwnerCommand('deferred', 2);
    bDeferredBefore = _mapInt(bDeferred, 'before');
    bDeferredAfter = _mapInt(bDeferred, 'after');
    bDeferredValue = _mapInt(bDeferred, 'value');
    DartPlantNative.resetInstrumentedAddProbe();
    aHookWhileBAfterDeferred = instrumentedAdd(2, 3);

    aPostDeferredEpoch = DartPlantNative.multiOwnerActivate(1);
    aHookAfterDeferred = instrumentedAdd(2, 3);

    await _launchChannel.invokeMethod<void>('multiOwnerDestroy');
    aPostDestroyEpoch = DartPlantNative.multiOwnerActivate(1);

    final recreate =
        await _launchChannel.invokeMethod<Object?>('multiOwnerRecreate');
    if (recreate is Map) {
      engineIncarnation2 =
          recreate['incarnation'] is int ? recreate['incarnation'] as int : 0;
    }
    final b2Activate = await _multiOwnerCommand('activate', 3);
    b2Epoch = _mapInt(b2Activate, 'epoch');
    final b2Hook = await _launchChannel.invokeMethod<Object?>(
      'multiOwnerCommand',
      <String, Object?>{'command': 'hookTarget', 'label': 3},
    );
    b2HookValue = b2Hook is int ? b2Hook : -1;
    DartPlantNative.resetInstrumentedAddProbe();
    aHookWhileB2Active = instrumentedAdd(2, 3);

    await _launchChannel.invokeMethod<void>('multiOwnerDestroy');
    aFinalEpoch = DartPlantNative.multiOwnerActivate(1);
    aHookFinal = instrumentedAdd(2, 3);
  } catch (error, stackTrace) {
    failure = '$error';
    debugPrint(
        'DartPlant multi-engine lifecycle exception: $error\n$stackTrace');
  } finally {
    try {
      await _launchChannel.invokeMethod<void>('multiOwnerDestroy');
    } catch (_) {
      // The engine may already be gone after a successful lifecycle run.
    }
  }

  final passed = failure == null &&
      aEpoch != 0 &&
      bEpoch != 0 &&
      b2Epoch != 0 &&
      aEpoch != bEpoch &&
      bEpoch != b2Epoch &&
      aEpoch != b2Epoch &&
      aReturnEpoch == aEpoch &&
      aPostDeferredEpoch == aEpoch &&
      aPostDestroyEpoch == aEpoch &&
      aFinalEpoch == aEpoch &&
      bGcBefore == bEpoch &&
      bGcAfter == bEpoch &&
      bDeferredBefore == bEpoch &&
      bDeferredAfter == bEpoch &&
      bDeferredValue == 42 &&
      engineIncarnation1 != 0 &&
      engineIncarnation2 > engineIncarnation1 &&
      aHookBefore == 115 &&
      aHookWhileBActive == 115 &&
      aHookAfterB == 115 &&
      aHookWhileBAfterDeferred == 115 &&
      aHookAfterDeferred == 115 &&
      aHookWhileB2Active == 115 &&
      aHookFinal == 115 &&
      bHookValue == 5 &&
      b2HookValue == 5;
  _ciScenario('multi_engine', passed, <String, Object?>{
    'a_epoch': aEpoch,
    'b_epoch': bEpoch,
    'b2_epoch': b2Epoch,
    'a_return_epoch': aReturnEpoch,
    'a_post_deferred_epoch': aPostDeferredEpoch,
    'a_post_destroy_epoch': aPostDestroyEpoch,
    'a_final_epoch': aFinalEpoch,
    'b_gc_before': bGcBefore,
    'b_gc_after': bGcAfter,
    'b_deferred_before': bDeferredBefore,
    'b_deferred_after': bDeferredAfter,
    'b_deferred_value': bDeferredValue,
    'engine_incarnation_1': engineIncarnation1,
    'engine_incarnation_2': engineIncarnation2,
    'a_hook_before': aHookBefore,
    'a_hook_while_b_active': aHookWhileBActive,
    'a_hook_after_b': aHookAfterB,
    'a_hook_while_b_after_deferred': aHookWhileBAfterDeferred,
    'a_hook_after_deferred': aHookAfterDeferred,
    'a_hook_while_b2_active': aHookWhileB2Active,
    'a_hook_final': aHookFinal,
    'b_hook_value': bHookValue,
    'b2_hook_value': b2HookValue,
    if (failure != null) 'error': failure,
  });
  return passed;
}

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
  _ciEvent('runtime', <String, Object?>{
    'flutter': _ciFlutterVersion,
    'dart': _ciDartVersion,
    'dart_runtime': Platform.version.split(' ').first,
    'abi': _ciTargetAbi,
    'dart_ffi_abi': Abi.current().toString(),
  });

  // Top-level tear-offs are lazily initialized. Force this generic implicit
  // closure into the live object graph before DartPlant builds its VM Function
  // index, then keep the same object strongly reachable for the later proof.
  _bootstrapRetainedGenericClosure = retainedGenericClosure;
  debugPrint(
    'DartPlant app retained generic closure bootstrap: ${_bootstrapRetainedGenericClosure != null ? 1 : 0}',
  );

  runApp(const DartPlantFixtureApp());

  // Do not stall the UI isolate before runApp while the live-VM sampler is
  // looking for a Dart mutator context. A real Flutter application
  // keeps executing Dart during startup, so the fixture should exercise the
  // bootstrap under the same workload instead of an artificial await-only
  // event loop.
  WidgetsBinding.instance.addPostFrameCallback((_) async {
    // Start native bootstrap only after this FlutterEngine has rendered its
    // first frame. Android may create and discard an earlier root engine in the
    // same process during startup; installing a process-wide AOT patch from
    // that short-lived IsolateGroup would bind every logical listener to a
    // stale owner receipt before the durable UI engine begins the test.
    final initializeStartStatus = DartPlantNative.startInitialize();
    final initializeStatus = initializeStartStatus == 0
        ? await DartPlantNative.waitForInitialization()
        : initializeStartStatus;
    var requestedTest = 'all';
    if (Platform.isAndroid) {
      final explicitTest =
          await _launchChannel.invokeMethod<String>('launchTest');
      final legacyProbe =
          await _launchChannel.invokeMethod<String>('launchProbe');
      if (explicitTest != null && explicitTest.isNotEmpty) {
        requestedTest = explicitTest;
      } else if (legacyProbe == 'type_arguments') {
        requestedTest = 'generic_gc';
      }
      debugPrint('DartPlant app launch probe: $requestedTest');
    }
    final validRequestedTest =
        requestedTest == 'all' || _ciRuntimeTests.contains(requestedTest);
    _ciEvent('test_begin', <String, Object?>{
      'name': requestedTest,
      'state': validRequestedTest ? 'accepted' : 'rejected',
    });
    if (!validRequestedTest) {
      _ciEvent('suite', <String, Object?>{
        'state': 'fail',
        'test': requestedTest,
        'reason': 'unknown dartplant_test',
      });
      return;
    }
    bool wants(String name) => requestedTest == 'all' || requestedTest == name;

    debugPrint('DartPlant initialize status: $initializeStatus');
    _ciScenario('initialization', initializeStatus == 0, <String, Object?>{
      'status': initializeStatus,
    });

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
    _ciScenario('local_gate', localGateWarmupPassed, <String, Object?>{
      'value': localGateWarmup,
    });

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
    _ciScenario('simple_facade', simpleFacadePassed, <String, Object?>{
      'install': simpleFacadeInstall,
      'stage1': simpleFacadeStage1,
      'stage2': simpleFacadeStage2,
    });

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
    final gcPort = ReceivePort();
    await Isolate.spawn(movingGcPressure, gcPort.sendPort);
    final gcEvents = StreamIterator<Object?>(gcPort);
    await gcEvents.moveNext();
    final p6HookedObjectPair = verifiedAbiPair(
      const FixtureObject(31),
      const FixtureObject(32),
    );
    await gcEvents.moveNext();
    await gcEvents.cancel();
    gcPort.close();
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
        p6HookedPair.$2 == 31 &&
        p6HookedObjectPair.$1 == const FixtureObject(32) &&
        p6HookedObjectPair.$2 == const FixtureObject(31);
    debugPrint(
      'DartPlant P6 ABI corpus: ${p6Passed ? 1 : 0} install=$p6Install probe=$p6Probe int64=$p6BaselineInt64/$p6HookedInt64 stack=$p6BaselineStack/$p6HookedStack odd=$p6BaselineOdd/$p6HookedOdd forced=$p6BaselineForced/$p6HookedForced pair=${p6BaselinePair.$1},${p6BaselinePair.$2}/${p6HookedPair.$1},${p6HookedPair.$2}',
    );
    _ciScenario('p6_abi', p6Passed, <String, Object?>{
      'install': p6Install,
      'probe': p6Probe,
      'throw_path': p6ThrowPath,
    });

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
    _ciScenario('exception_bridge', exceptionLifetimePassed, <String, Object?>{
      'install': exceptionLifetimeInstall,
      'catch': exceptionLifetimeCatch,
      'probe': exceptionLifetimeProbe,
    });

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
    _ciScenario('closure_receiver', forcedStackClosurePassed, <String, Object?>{
      'install': forcedStackClosureInstall,
      'native': forcedStackClosureProbe,
      'value': forcedStackClosureValue,
    });

    // Only after the simple consumer has removed its final subscription and
    // the P6/exception consumers have removed all artifact-first hooks and
    // shut down may the advanced fixture reuse physical entry targets for its
    // ABI/late-shared diagnostics.
    final advancedOrdinaryHook = DartPlantNative.enableAdvancedOrdinaryHook();
    debugPrint(
        'DartPlant advanced ordinary hook enable: $advancedOrdinaryHook');
    _ciScenario(
        'advanced_ordinary', advancedOrdinaryHook == 0, <String, Object?>{
      'status': advancedOrdinaryHook,
    });

    DartPlantNative.resetNullSemanticProbe();
    final canonicalNull = nullableEchoObject(null);
    final rewrittenToNull = nullableEchoObject(const FixtureObject(11));
    final nullProbe = DartPlantNative.nullSemanticProbe();
    debugPrint(
      'DartPlant null semantic probe: $nullProbe values=$canonicalNull/$rewrittenToNull',
    );
    _ciScenario(
      'null_semantics',
      nullProbe == 1 && canonicalNull == null && rewrittenToNull == null,
      <String, Object?>{'native': nullProbe},
    );
    DartPlantNative.resetBoolSemanticProbe();
    // Keep the caller-side expectation runtime-dependent. PRODUCT AOT is free
    // to reason about constant arguments even when the callee is never-inline;
    // this proof must validate the architectural return value written by the
    // leave callback rather than a caller constant-folding opportunity.
    final boolSeed = Platform.numberOfProcessors > 0;
    final boolFirstInput = !boolSeed;
    final boolSecondInput = boolSeed;
    final boolFirst = negateBool(boolFirstInput);
    final boolSecond = negateBool(boolSecondInput);
    final boolProbe = DartPlantNative.boolSemanticProbe();
    final boolSemanticPassed = boolProbe == 1 &&
        boolFirst == boolFirstInput &&
        boolSecond == boolSecondInput;
    debugPrint(
      'DartPlant bool semantic probe: $boolProbe inputs=$boolFirstInput/$boolSecondInput values=$boolFirst/$boolSecond',
    );
    _ciScenario(
      'bool_semantics',
      boolSemanticPassed,
      <String, Object?>{
        'native': boolProbe,
        'first_input': boolFirstInput,
        'second_input': boolSecondInput,
        'first_result': boolFirst,
        'second_result': boolSecond,
      },
    );
    DartPlantNative.resetInstrumentedAddProbe();
    for (var index = 0; index < 5; ++index) {
      instrumentedAdd(2, 3);
    }
    final startupProbe = DartPlantNative.instrumentedAddProbe();
    debugPrint('DartPlant live VM startup probe: $startupProbe');
    _ciScenario('live_vm_startup', startupProbe == 115, <String, Object?>{
      'value': startupProbe,
    });
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
    final ordinaryPassed = ordinaryProbe == 1 &&
        ordinaryDirect == 16.125 &&
        ordinaryAfterShared == 6.25;
    _ciScenario('ordinary_aot', ordinaryPassed, <String, Object?>{
      'native': ordinaryProbe,
    });
    final lateSharedPassed = lateSharedTransition == 1 && ordinaryPassed;
    debugPrint(
      'DartPlant late shared typed fail-close: ${lateSharedPassed ? 1 : 0} transition=$lateSharedTransition values=$ordinaryDirect/$ordinaryAfterShared',
    );
    _ciScenario('late_shared', lateSharedPassed, <String, Object?>{
      'transition': lateSharedTransition,
    });

    final normalPassed = initializeStatus == 0 &&
        localGateWarmupPassed &&
        simpleFacadePassed &&
        advancedOrdinaryHook == 0 &&
        nullProbe == 1 &&
        canonicalNull == null &&
        rewrittenToNull == null &&
        boolSemanticPassed &&
        startupProbe == 115 &&
        ordinaryPassed;
    if (wants('normal')) {
      _ciScenario('normal', normalPassed, <String, Object?>{
        'initialize': initializeStatus,
        'startup': startupProbe,
        'ordinary': ordinaryProbe,
      });
    }

    if (wants('closure')) {
      _ciScenario('closure', forcedStackClosurePassed, <String, Object?>{
        'install': forcedStackClosureInstall,
        'native': forcedStackClosureProbe,
      });
    }

    if (wants('exception')) {
      _ciScenario(
        'exception',
        exceptionLifetimePassed && p6ThrowPath == 2,
        <String, Object?>{
          'lifetime': exceptionLifetimeProbe,
          'catch_path': exceptionLifetimeCatch,
          'throw_path': p6ThrowPath,
        },
      );
    }

    if (wants('arguments_descriptor')) {
      _runTypeArgumentsProof(
        'ci_arguments_descriptor',
        scenario: 'arguments_descriptor',
        requireRelocation: false,
      );
    }
    if (wants('generic_closure')) {
      _runTypeArgumentsProof(
        'ci_generic_closure',
        scenario: 'generic_closure',
        requireRelocation: false,
      );
    }
    if (wants('generic_gc')) {
      _runTypeArgumentsProof(
        'adb',
        scenario: 'generic_gc',
        requireRelocation: true,
      );
    }

    final transitionPassed = DartPlantNative.transitionProof() == 1;
    if (wants('transition')) {
      _ciScenario('transition', transitionPassed);
    }
    final artifactRevalidatePassed =
        DartPlantNative.artifactLifecycleProof() == 1;
    if (wants('artifact_revalidate')) {
      _ciScenario('artifact_revalidate', artifactRevalidatePassed);
    }
    final snapshotOffsetPassed = DartPlantNative.snapshotOffsetProof() == 31;
    if (wants('normal')) {
      _ciScenario('snapshot_offset', snapshotOffsetPassed, <String, Object?>{
        'proof': DartPlantNative.snapshotOffsetProof(),
      });
    }

    if (wants('deferred_lifecycle')) {
      final beforeLoad = DartPlantNative.deferredBeforeLoad();
      await deferred_probe.loadLibrary();
      final afterLoad = DartPlantNative.deferredAfterLoad();
      final value = deferred_probe.deferredAdd(1);
      DartPlantNative.resetInstrumentedAddProbe();
      var postDeferredInstrumented = 0;
      for (var index = 0; index < 5; ++index) {
        postDeferredInstrumented = instrumentedAdd(2, 3);
      }
      final postDeferredInstrumentedNative =
          DartPlantNative.instrumentedAddProbe();
      _runTypeArgumentsProof(
        'post_deferred',
        scenario: 'deferred_type_arguments_post_load',
        requireRelocation: false,
      );
      final postDeferredTypeArguments =
          _ciScenarioResults['deferred_type_arguments_post_load'] == true;
      _ciScenario(
        'deferred_lifecycle',
        beforeLoad == 1 &&
            afterLoad == 1 &&
            value == 42 &&
            postDeferredInstrumented == 115 &&
            postDeferredInstrumentedNative == 115 &&
            postDeferredTypeArguments,
        <String, Object?>{
          'before_load': beforeLoad,
          'after_load': afterLoad,
          'value': value,
          'post_deferred_instrumented': postDeferredInstrumented,
          'post_deferred_instrumented_native': postDeferredInstrumentedNative,
          'post_deferred_type_arguments': postDeferredTypeArguments,
        },
      );
    }

    if (wants('multi_engine')) {
      await _runMultiEngineLifecycleProof();
    }

    final commonNativeProofPassed =
        _ciCommonScenarios.every((name) => _ciScenarioResults[name] == true);
    final selectedNativeProofPassed = commonNativeProofPassed &&
        (requestedTest == 'all'
            ? _ciRuntimeTests.every((name) => _ciScenarioResults[name] == true)
            : _ciScenarioResults[requestedTest] == true);
    _ciEvent('suite', <String, Object?>{
      'state': selectedNativeProofPassed ? 'pass' : 'fail',
      'test': requestedTest,
    });
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

  void _runTypeArgumentsButtonProbe() {
    _show(_runTypeArgumentsProof('ui'));
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
              key: const ValueKey('fixture-type-arguments'),
              onPressed: _runTypeArgumentsButtonProbe,
              child: const Text('TypeArguments GC proof'),
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
