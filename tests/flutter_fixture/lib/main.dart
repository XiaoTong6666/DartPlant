import 'package:flutter/material.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'dartplant_native.dart';
import 'external_module_native.dart';
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
  'changed_slotless_reject',
};
const _ciCommonScenarios = <String>{
  'initialization',
  'layout_binding',
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
      case 'externalOwnerCall':
        // A's external Hook is installed in the original Engine owner.
        // This call runs in B/B2's real Dart mutator and must not be routed
        // into A's logical listener simply because AOT code is shared.
        return <String, Object?>{'value': instrumentedAdd(2, 3)};
      case 'activate':
        return <String, Object?>{
          'epoch': DartPlantNative.multiOwnerActivate(label),
        };
      case 'hookTarget':
        final install = DartPlantNative.multiOwnerInstallListener(label);
        final value = instrumentedAdd(2, 3);
        final probe = DartPlantNative.multiOwnerListenerProbe(label);
        return <String, Object?>{
          'install': install,
          'value': value,
          'probe': probe,
        };
      case 'hookUnloadRace':
        final install = DartPlantNative.multiOwnerInstallListener(label);
        final started =
            install == 1 ? DartPlantNative.multiOwnerRetireRaceStart(label) : 0;
        final value = started == 1 ? instrumentedAdd(2, 3) : -1;
        final probe =
            started == 1 ? DartPlantNative.multiOwnerRetireRaceProbe(label) : 0;
        return <String, Object?>{
          'install': install,
          'started': started,
          'value': value,
          'probe': probe,
        };
      case 'hookThrow':
        final install = DartPlantNative.multiOwnerExceptionInstall(label);
        var caught = 0;
        if (install == 1) {
          try {
            verifiedAbiThrowingStack(99, 2, 3, 4, 5, 6, 7, 8);
          } on StateError catch (error) {
            caught = error.message == 'dartplant-p6-throw' ? 1 : 0;
          }
        }
        final probe =
            install == 1 ? DartPlantNative.multiOwnerExceptionProbe(label) : 0;
        return <String, Object?>{
          'install': install,
          'caught': caught,
          'probe': probe,
        };
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

// Kept as a separate external exception entry. The unannotated P6 throwing
// callee retains its optimized compiler ABI and independent test contract.
@pragma('vm:entry-point')
@pragma('vm:never-inline')
int externalThrowingProbe(int value) {
  if (value == 99) throw StateError('dartplant-external-throw');
  return value + 1;
}

@pragma('vm:entry-point')
@pragma('vm:never-inline')
FixtureObject externalObjectRootProbe(int seed) {
  // A fresh heap object, not a canonical const; the caller only retains its
  // scalar value, leaving the external strong VM handle as its durable root.
  return FixtureObject(seed + 31);
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
  var bListenerInstall = 0;
  var bListenerProbe = 0;
  var b2ListenerInstall = 0;
  var b2ListenerProbe = 0;
  var aExceptionInstall = 0;
  var aExceptionBaseline = 0.0;
  var bExceptionInstall = 0;
  var bExceptionCatch = 0;
  var bExceptionProbe = 0;
  var b2ExceptionInstall = 0;
  var b2ExceptionCatch = 0;
  var b2ExceptionProbe = 0;
  var bRetireEpoch = 0;
  var bRetireInstall = 0;
  var bRetireStart = 0;
  var bRetireValue = -1;
  var bRetireProbe = 0;
  var aAfterBRetire = 0;
  var engineIncarnation1 = 0;
  var engineIncarnation2 = 0;
  String? failure;

  try {
    DartPlantNative.resetInstrumentedAddProbe();
    aEpoch = DartPlantNative.multiOwnerActivate(1);
    aHookBefore = instrumentedAdd(2, 3);
    aExceptionInstall = DartPlantNative.multiOwnerExceptionPrepare();
    if (aExceptionInstall == 1) {
      aExceptionBaseline = verifiedAbiThrowingStack(1, 2, 3, 4, 5, 6, 7, 8);
    }

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
    if (bHook is Map) {
      final values = Map<Object?, Object?>.from(bHook);
      bHookValue = _mapInt(values, 'value');
      bListenerInstall = _mapInt(values, 'install');
      bListenerProbe = _mapInt(values, 'probe');
    }
    final bThrow = await _multiOwnerCommand('hookThrow', 2);
    bExceptionInstall = _mapInt(bThrow, 'install');
    bExceptionCatch = _mapInt(bThrow, 'caught');
    bExceptionProbe = _mapInt(bThrow, 'probe');
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

    // B is the active VM owner during this call. A background native thread
    // executes production owner retirement while B's Dart enter callback is
    // held in-flight; B then finishes through the original physical RET.
    final bRetireActivation = await _multiOwnerCommand('activate', 2);
    bRetireEpoch = _mapInt(bRetireActivation, 'epoch');
    final bRace = await _multiOwnerCommand('hookUnloadRace', 2);
    bRetireInstall = _mapInt(bRace, 'install');
    bRetireStart = _mapInt(bRace, 'started');
    bRetireValue = _mapInt(bRace, 'value');
    bRetireProbe = _mapInt(bRace, 'probe');
    aAfterBRetire = instrumentedAdd(2, 3);

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
    if (b2Hook is Map) {
      final values = Map<Object?, Object?>.from(b2Hook);
      b2HookValue = _mapInt(values, 'value');
      b2ListenerInstall = _mapInt(values, 'install');
      b2ListenerProbe = _mapInt(values, 'probe');
    }
    final b2Throw = await _multiOwnerCommand('hookThrow', 3);
    b2ExceptionInstall = _mapInt(b2Throw, 'install');
    b2ExceptionCatch = _mapInt(b2Throw, 'caught');
    b2ExceptionProbe = _mapInt(b2Throw, 'probe');
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
    DartPlantNative.multiOwnerExceptionCleanup();
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
      bRetireEpoch == bEpoch &&
      bRetireInstall == 1 &&
      bRetireStart == 1 &&
      bRetireValue == 5 &&
      bRetireProbe == 1 &&
      aAfterBRetire == 115 &&
      aHookWhileB2Active == 115 &&
      aHookFinal == 115 &&
      bHookValue == 5 &&
      b2HookValue == 5 &&
      bListenerInstall == 1 &&
      bListenerProbe == 1 &&
      b2ListenerInstall == 1 &&
      b2ListenerProbe == 1 &&
      aExceptionInstall == 1 &&
      aExceptionBaseline == 108.0 &&
      bExceptionInstall == 1 &&
      bExceptionCatch == 1 &&
      bExceptionProbe == 1 &&
      b2ExceptionInstall == 1 &&
      b2ExceptionCatch == 1 &&
      b2ExceptionProbe == 1;
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
    'b_listener_install': bListenerInstall,
    'b_listener_probe': bListenerProbe,
    'b2_listener_install': b2ListenerInstall,
    'b2_listener_probe': b2ListenerProbe,
    'a_exception_install': aExceptionInstall,
    'a_exception_baseline': aExceptionBaseline,
    'b_exception_install': bExceptionInstall,
    'b_exception_catch': bExceptionCatch,
    'b_exception_probe': bExceptionProbe,
    'b_retire_epoch': bRetireEpoch,
    'b_retire_install': bRetireInstall,
    'b_retire_start': bRetireStart,
    'b_retire_value': bRetireValue,
    'b_retire_probe': bRetireProbe,
    'a_after_b_retire': aAfterBRetire,
    'b2_exception_install': b2ExceptionInstall,
    'b2_exception_catch': b2ExceptionCatch,
    'b2_exception_probe': b2ExceptionProbe,
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
  if (const bool.fromEnvironment('DARTPLANT_EXTERNAL_MODULE')) {
    runApp(const DartPlantFixtureApp());
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      final report = await externalModuleChannel
          .invokeMapMethod<String, Object?>('externalModuleProbe');
      final value = report?['value'] as int? ?? 0;
      final entry = report?['entry'] as int? ?? 0;
      final counters = report?['counters'] as int? ?? 0;
      final retireEntry = report?['retire'] as int? ?? 0;
      final mappingEntry = report?['mapping'] as int? ?? 0;
      final exceptionEntry = report?['exception'] as int? ?? 0;
      final objectRootEntry = report?['objectRoot'] as int? ?? 0;
      final loaderDrainEntry = report?['loaderDrain'] as int? ?? 0;
      final externalExceptionBaseline =
          entry != 0 ? externalThrowingProbe(1) : -1;
      final bootstrap = entry == 0 ? 0 : ExternalModuleNative.bootstrap(entry);
      final hookedResult = bootstrap == 1 ? instrumentedAdd(2, 3) : 0;
      final counts = counters != 0 && bootstrap == 1
          ? ExternalModuleNative.counters(counters)
          : 0;
      // These hooks are owned and installed by libdartplant.so in the
      // injected module. Do not import the embedded fixture's test runtime.
      final externalCanonicalNull =
          bootstrap == 1 ? nullableEchoObject(null) : const FixtureObject(-2);
      final externalRewrittenNull = bootstrap == 1
          ? nullableEchoObject(const FixtureObject(11))
          : const FixtureObject(-2);
      final externalBoolSeed = Platform.numberOfProcessors > 0;
      final externalBoolFirstInput = !externalBoolSeed;
      final externalBoolSecondInput = externalBoolSeed;
      final externalBoolFirst = bootstrap == 1
          ? negateBool(externalBoolFirstInput)
          : !externalBoolFirstInput;
      final externalBoolSecond = bootstrap == 1
          ? negateBool(externalBoolSecondInput)
          : !externalBoolSecondInput;
      _ciScenario(
          'external_null_semantics',
          bootstrap == 1 &&
              externalCanonicalNull == null &&
              externalRewrittenNull == null,
          {
            'canonical_null': externalCanonicalNull == null,
            'rewritten_null': externalRewrittenNull == null,
          });
      _ciScenario(
          'external_bool_semantics',
          bootstrap == 1 &&
              externalBoolFirst == externalBoolFirstInput &&
              externalBoolSecond == externalBoolSecondInput,
          {
            'first_input': externalBoolFirstInput,
            'first_result': externalBoolFirst,
            'second_input': externalBoolSecondInput,
            'second_result': externalBoolSecond,
          });
      var externalExceptionCaught = 0;
      if (bootstrap == 1) {
        try {
          externalThrowingProbe(99);
          externalExceptionCaught = 1;
        } on StateError catch (error) {
          externalExceptionCaught =
              error.message == 'dartplant-external-throw' ? 2 : 3;
        } catch (_) {
          externalExceptionCaught = 3;
        }
      }
      final externalExceptionProbe = bootstrap == 1 && exceptionEntry != 0
          ? ExternalModuleNative.exception(exceptionEntry)
          : 0;
      _ciScenario(
          'external_exception',
          externalExceptionBaseline == 2 &&
              externalExceptionCaught == 2 &&
              externalExceptionProbe == 1,
          {
            'baseline': externalExceptionBaseline,
            'caught': externalExceptionCaught,
            'native': externalExceptionProbe,
            'object_api_available': false,
          });
      var externalRootFirst = -1;
      var externalRootSecond = -1;
      var externalRootProbe = 0;
      String? externalRootError;
      if (bootstrap == 1 && objectRootEntry != 0) {
        try {
          // Keep only the scalar here; the first returned object must stay
          // alive through the independent module's strong VM root.
          externalRootFirst = externalObjectRootProbe(7).value;
          await _multiOwnerGcPressure();
          externalRootSecond = externalObjectRootProbe(8).value;
          externalRootProbe = ExternalModuleNative.objectRoot(objectRootEntry);
        } catch (error) {
          externalRootError = '$error';
        }
      }
      _ciScenario(
          'external_object_root_gc',
          externalRootError == null &&
              externalRootFirst == 38 &&
              externalRootSecond == 39 &&
              (externalRootProbe & 1) != 0,
          {
            'first_value': externalRootFirst,
            'second_value': externalRootSecond,
            'strong_root_alive': (externalRootProbe & 1) != 0,
            'root_address_changed': (externalRootProbe & 2) != 0,
            'generic_typearguments_proven': false,
            'error': externalRootError ?? '',
          });
      final retired = bootstrap == 1 && retireEntry != 0
          ? ExternalModuleNative.retire(retireEntry)
          : 0;
      final unhookedResult = retired == 1 ? instrumentedAdd(2, 3) : 0;
      final idleCounts =
          counters != 0 ? ExternalModuleNative.counters(counters) : 0;
      final mappingControl = retired == 1 && mappingEntry != 0
          ? ExternalModuleNative.mapping(mappingEntry)
          : 0;
      final rebound = retired == 1 ? ExternalModuleNative.bootstrap(entry) : 0;
      final reboundResult = rebound == 1 ? instrumentedAdd(2, 3) : 0;
      final reboundCounts =
          counters != 0 ? ExternalModuleNative.counters(counters) : 0;
      var firstEngineIncarnation = 0;
      var secondEngineIncarnation = 0;
      var bValue = -1;
      var b2Value = -1;
      var aAfterBValue = -1;
      var aAfterB2Value = -1;
      var bCounts = 0;
      var b2Counts = 0;
      var aAfterBCounts = 0;
      var aAfterB2Counts = 0;
      String? externalOwnerError;
      if (rebound == 1 && counters != 0) {
        try {
          final start =
              await _launchChannel.invokeMethod<Object?>('multiOwnerStart');
          if (start is Map && start['incarnation'] is int) {
            firstEngineIncarnation = start['incarnation'] as int;
          }
          final b = await _multiOwnerCommand('externalOwnerCall', 2);
          bValue = _mapInt(b, 'value');
          bCounts = ExternalModuleNative.counters(counters);
          await _launchChannel.invokeMethod<void>('multiOwnerDestroy');
          aAfterBValue = instrumentedAdd(2, 3);
          aAfterBCounts = ExternalModuleNative.counters(counters);
          final recreate =
              await _launchChannel.invokeMethod<Object?>('multiOwnerRecreate');
          if (recreate is Map && recreate['incarnation'] is int) {
            secondEngineIncarnation = recreate['incarnation'] as int;
          }
          final b2 = await _multiOwnerCommand('externalOwnerCall', 3);
          b2Value = _mapInt(b2, 'value');
          b2Counts = ExternalModuleNative.counters(counters);
          await _launchChannel.invokeMethod<void>('multiOwnerDestroy');
          aAfterB2Value = instrumentedAdd(2, 3);
          aAfterB2Counts = ExternalModuleNative.counters(counters);
        } catch (error, stackTrace) {
          externalOwnerError = '$error';
          debugPrint(
              'DartPlant external owner lifecycle error: $error\n$stackTrace');
        } finally {
          try {
            await _launchChannel.invokeMethod<void>('multiOwnerDestroy');
          } catch (_) {
            // A previously destroyed Engine has no remaining owner.
          }
        }
      }
      _ciScenario(
          'external_owner_lifecycle',
          externalOwnerError == null &&
              firstEngineIncarnation != 0 &&
              secondEngineIncarnation > firstEngineIncarnation &&
              bValue == 5 &&
              bCounts == reboundCounts &&
              aAfterBValue == 115 &&
              aAfterBCounts == 0x300000003 &&
              b2Value == 5 &&
              b2Counts == aAfterBCounts &&
              aAfterB2Value == 115 &&
              aAfterB2Counts == 0x400000004,
          {
            'first_engine': firstEngineIncarnation,
            'second_engine': secondEngineIncarnation,
            'b_value': bValue,
            'b_counts': bCounts,
            'a_after_b': aAfterBValue,
            'a_after_b_counts': aAfterBCounts,
            'b2_value': b2Value,
            'b2_counts': b2Counts,
            'a_after_b2': aAfterB2Value,
            'a_after_b2_counts': aAfterB2Counts,
            'error': externalOwnerError ?? '',
          });
      var gcBefore = 0;
      var gcAfter = 0;
      var gcCountersBefore = 0;
      var gcCountersAfter = 0;
      String? gcError;
      if (rebound == 1 && counters != 0) {
        try {
          gcCountersBefore = ExternalModuleNative.counters(counters);
          gcBefore = instrumentedAdd(2, 3);
          // The allocations run on A's own mutator. This proves callback
          // survival across allocation pressure, not object-root relocation.
          await _multiOwnerGcPressure();
          gcAfter = instrumentedAdd(2, 3);
          gcCountersAfter = ExternalModuleNative.counters(counters);
        } catch (error) {
          gcError = '$error';
        }
      }
      _ciScenario(
          'external_gc_pressure',
          gcError == null &&
              gcBefore == 115 &&
              gcAfter == 115 &&
              (gcCountersAfter >> 32) == (gcCountersBefore >> 32) + 2 &&
              (gcCountersAfter & 0xffffffff) ==
                  (gcCountersBefore & 0xffffffff) + 2,
          {
            'before': gcBefore,
            'after': gcAfter,
            'enter_before': gcCountersBefore >> 32,
            'enter_after': gcCountersAfter >> 32,
            'leave_before': gcCountersBefore & 0xffffffff,
            'leave_after': gcCountersAfter & 0xffffffff,
            'object_root_relocation_proven': false,
            'error': gcError ?? '',
          });

      // Retire the rebound runtime first, then close the independent
      // Vector/LSPosed loader callback. Native API v2 has no unregister, so
      // libdartplant.so remains NODELETE even after the logical drain.
      final finalRetired = rebound == 1 && retireEntry != 0
          ? ExternalModuleNative.retire(retireEntry)
          : 0;
      final loaderDrained = finalRetired == 1 && loaderDrainEntry != 0
          ? ExternalModuleNative.retire(loaderDrainEntry)
          : 0;
      _ciScenario(
          'external_callback_drain',
          finalRetired == 1 && loaderDrained == 1,
          {
            'runtime_retired': finalRetired,
            'loader_callback_drained': loaderDrained,
            'nodelete_required': true,
          });
      _ciScenario(
          'external_module',
          value == 107 &&
              bootstrap == 1 &&
              hookedResult == 115 &&
              counts == 0x100000001 &&
              retired == 1 &&
              unhookedResult == 5 &&
              idleCounts == counts &&
              mappingControl == 1 &&
              rebound == 1 &&
              reboundResult == 115 &&
              reboundCounts == 0x200000002,
          {
            'java_value': value,
            'entry_present': entry != 0,
            'bootstrap': bootstrap,
            'dart_result': hookedResult,
            'dart_enter': counts >> 32,
            'dart_leave': counts & 0xffffffff,
            'retired': retired,
            'unhooked_result': unhookedResult,
            'idle_counts': idleCounts,
            'physical_mapping_control': mappingControl,
            'rebound': rebound,
            'rebound_result': reboundResult,
            'rebound_enter': reboundCounts >> 32,
            'rebound_leave': reboundCounts & 0xffffffff,
          });
      _ciEvent('suite', {
        'state': _ciScenarioResults['external_module'] == true &&
                _ciScenarioResults['external_owner_lifecycle'] == true &&
                _ciScenarioResults['external_null_semantics'] == true &&
                _ciScenarioResults['external_bool_semantics'] == true &&
                _ciScenarioResults['external_exception'] == true &&
                _ciScenarioResults['external_object_root_gc'] == true &&
                _ciScenarioResults['external_gc_pressure'] == true &&
                _ciScenarioResults['external_callback_drain'] == true
            ? 'pass'
            : 'fail',
        'mode': 'external_module_subset',
        'dobby_parity_complete': false,
      });
    });
    return;
  }
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

    // A synthetic native dispatch in the release Flutter process validates
    // that identical VM proof receipts do not merge incompatible per-listener
    // call layouts. This is separate from the real-AOT multi-engine fixture:
    // production method layouts are never deliberately falsified.
    final layoutBindingProbe = DartPlantNative.callLayoutBindingProbe();
    _ciScenario('layout_binding', layoutBindingProbe == 1, <String, Object?>{
      'native': layoutBindingProbe,
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

    if (wants('deferred_lifecycle') || wants('changed_slotless_reject')) {
      final beforeLoad = DartPlantNative.deferredBeforeLoad();
      await deferred_probe.loadLibrary();
      final afterLoad = DartPlantNative.deferredAfterLoad();
      final value = deferred_probe.deferredAdd(1);
      final changedSlotlessReject =
          DartPlantNative.changedSlotlessRejectProbe();
      _ciScenario('changed_slotless_reject', changedSlotlessReject == 1, {
        'native': changedSlotlessReject,
      });
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
