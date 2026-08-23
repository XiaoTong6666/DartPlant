import 'dart:ffi';

typedef _InitializeNative = Int32 Function(Pointer<Void> apiData);
typedef _InitializeDart = int Function(Pointer<Void> apiData);
typedef _ColdBootstrapStatusNative = Int32 Function();
typedef _ColdBootstrapStatusDart = int Function();
typedef _ShutdownNative = Void Function();
typedef _ShutdownDart = void Function();
typedef _BeginObjectProbeNative = Void Function();
typedef _BeginObjectProbeDart = void Function();
typedef _ReleaseObjectRootNative = Void Function();
typedef _ReleaseObjectRootDart = void Function();
typedef _ResetInstrumentedAddProbeNative = Void Function();
typedef _ResetInstrumentedAddProbeDart = void Function();
typedef _InstrumentedAddProbeNative = Uint64 Function();
typedef _InstrumentedAddProbeDart = int Function();
typedef _ResetNullSemanticProbeNative = Void Function();
typedef _ResetNullSemanticProbeDart = void Function();
typedef _NullSemanticProbeNative = Uint64 Function();
typedef _NullSemanticProbeDart = int Function();

final class DartPlantNative {
  DartPlantNative._();

  static final DynamicLibrary _library =
      DynamicLibrary.open('libdartplant_fixture_bridge.so');
  static final _InitializeDart _initialize = _library
      .lookup<NativeFunction<_InitializeNative>>(
        'dartplant_fixture_initialize',
      )
      .asFunction();
  static final _ColdBootstrapStatusDart _coldBootstrapStatus = _library
      .lookup<NativeFunction<_ColdBootstrapStatusNative>>(
        'dartplant_fixture_cold_bootstrap_status',
      )
      .asFunction();
  static final _ShutdownDart _shutdown = _library
      .lookup<NativeFunction<_ShutdownNative>>('dartplant_fixture_shutdown')
      .asFunction();
  static final _BeginObjectProbeDart _beginObjectProbe = _library
      .lookup<NativeFunction<_BeginObjectProbeNative>>(
        'dartplant_fixture_begin_object_probe',
      )
      .asFunction();
  static final _ReleaseObjectRootDart _releaseObjectRoot = _library
      .lookup<NativeFunction<_ReleaseObjectRootNative>>(
        'dartplant_fixture_release_object_root',
      )
      .asFunction();
  static final _ResetInstrumentedAddProbeDart _resetInstrumentedAddProbe =
      _library
          .lookup<NativeFunction<_ResetInstrumentedAddProbeNative>>(
            'dartplant_fixture_reset_instrumented_add_probe',
          )
          .asFunction();
  static final _InstrumentedAddProbeDart _instrumentedAddProbe = _library
      .lookup<NativeFunction<_InstrumentedAddProbeNative>>(
        'dartplant_fixture_instrumented_add_probe',
      )
           .asFunction();
  static final _ResetNullSemanticProbeDart _resetNullSemanticProbe = _library
      .lookup<NativeFunction<_ResetNullSemanticProbeNative>>(
        'dartplant_fixture_reset_null_semantic_probe',
      )
      .asFunction();
  static final _NullSemanticProbeDart _nullSemanticProbe = _library
      .lookup<NativeFunction<_NullSemanticProbeNative>>(
        'dartplant_fixture_null_semantic_probe',
      )
      .asFunction();

  static int startInitialize() => _initialize(NativeApi.initializeApiDLData);

  @pragma('vm:never-inline')
  static int _bootstrapHeartbeat(int seed) {
    var value = seed;
    for (var index = 0; index < 2048; ++index) {
      value = ((value * 33) ^ index) & 0x7fffffff;
    }
    return value;
  }

  static Future<int> waitForInitialization() async {
    var heartbeat = 1;
    for (var attempt = 0; attempt < 1000; ++attempt) {
      // Keep the UI isolate executing ordinary AOT Dart while the native
      // The live-VM sampler validates THR/PP/HEAP_BITS/NULL. This supplies
      // mutator activity only; no method address or metadata crosses the FFI.
      for (var burst = 0; burst < 32; ++burst) {
        heartbeat = _bootstrapHeartbeat(heartbeat + burst);
      }
      final status = _coldBootstrapStatus();
      if (status >= 0) return status;
      await Future<void>.delayed(const Duration(milliseconds: 1));
    }
    return heartbeat == -1 ? heartbeat : 14; // DARTPLANT_RUNTIME_NOT_READY
  }

  static Future<int> initialize() async {
    final startStatus = startInitialize();
    return startStatus == 0 ? waitForInitialization() : startStatus;
  }

  static void shutdown() => _shutdown();

  static void beginObjectProbe() => _beginObjectProbe();

  static void releaseObjectRoot() => _releaseObjectRoot();

  static void resetInstrumentedAddProbe() => _resetInstrumentedAddProbe();

  static int instrumentedAddProbe() => _instrumentedAddProbe();

  static void resetNullSemanticProbe() => _resetNullSemanticProbe();

  static int nullSemanticProbe() => _nullSemanticProbe();
}
