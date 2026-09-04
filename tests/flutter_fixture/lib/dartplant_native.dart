import 'dart:ffi';

typedef _InitializeNative = Int32 Function(Pointer<Void> apiData);
typedef _InitializeDart = int Function(Pointer<Void> apiData);
typedef _ColdBootstrapStatusNative = Int32 Function();
typedef _ColdBootstrapStatusDart = int Function();
typedef _SimpleFacadeInstallNative = Int32 Function();
typedef _SimpleFacadeInstallDart = int Function();
typedef _SimpleFacadeStageNative = Uint64 Function();
typedef _SimpleFacadeStageDart = int Function();
typedef _EnableAdvancedOrdinaryHookNative = Int32 Function();
typedef _EnableAdvancedOrdinaryHookDart = int Function();
typedef _EnableForcedStackClosureHookNative = Int32 Function();
typedef _EnableForcedStackClosureHookDart = int Function();
typedef _P6AbiInstallNative = Int32 Function();
typedef _P6AbiInstallDart = int Function();
typedef _P6AbiProbeNative = Uint64 Function();
typedef _P6AbiProbeDart = int Function();
typedef _ForcedStackClosureProbeNative = Uint64 Function();
typedef _ForcedStackClosureProbeDart = int Function();
typedef _TypeArgumentsProofPrepareNative = Int32 Function(Handle);
typedef _TypeArgumentsProofPrepareDart = int Function(Object);
typedef _TypeArgumentsProofNative = Uint64 Function();
typedef _TypeArgumentsProofDart = int Function();
typedef _ExceptionBridgeLifetimeInstallNative = Int32 Function();
typedef _ExceptionBridgeLifetimeInstallDart = int Function();
typedef _ExceptionBridgeLifetimeProbeNative = Uint64 Function();
typedef _ExceptionBridgeLifetimeProbeDart = int Function();
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
typedef _ResetBoolSemanticProbeNative = Void Function();
typedef _ResetBoolSemanticProbeDart = void Function();
typedef _BoolSemanticProbeNative = Uint64 Function();
typedef _BoolSemanticProbeDart = int Function();
typedef _ResetVerifiedAbiDoubleProbeNative = Void Function();
typedef _ResetVerifiedAbiDoubleProbeDart = void Function();
typedef _VerifiedAbiDoubleProbeNative = Uint64 Function();
typedef _VerifiedAbiDoubleProbeDart = int Function();
typedef _MarkVerifiedAbiDoubleSharedNative = Uint64 Function();
typedef _MarkVerifiedAbiDoubleSharedDart = int Function();

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
  static final _SimpleFacadeInstallDart _simpleFacadeInstall = _library
      .lookup<NativeFunction<_SimpleFacadeInstallNative>>(
        'dartplant_fixture_simple_facade_install',
      )
      .asFunction();
  static final _SimpleFacadeStageDart _simpleFacadeStage1 = _library
      .lookup<NativeFunction<_SimpleFacadeStageNative>>(
        'dartplant_fixture_simple_facade_stage1',
      )
      .asFunction();
  static final _SimpleFacadeStageDart _simpleFacadeStage2 = _library
      .lookup<NativeFunction<_SimpleFacadeStageNative>>(
        'dartplant_fixture_simple_facade_stage2',
      )
      .asFunction();
  static final _EnableAdvancedOrdinaryHookDart _enableAdvancedOrdinaryHook =
      _library
          .lookup<NativeFunction<_EnableAdvancedOrdinaryHookNative>>(
            'dartplant_fixture_enable_advanced_ordinary_hook',
          )
          .asFunction();
  static final _EnableForcedStackClosureHookDart _enableForcedStackClosureHook =
      _library
          .lookup<NativeFunction<_EnableForcedStackClosureHookNative>>(
            'dartplant_fixture_enable_forced_stack_closure_hook',
          )
          .asFunction();
  static final _P6AbiInstallDart _p6AbiInstall = _library
      .lookup<NativeFunction<_P6AbiInstallNative>>(
        'dartplant_fixture_p6_abi_install',
      )
      .asFunction();
  static final _P6AbiProbeDart _p6AbiProbe = _library
      .lookup<NativeFunction<_P6AbiProbeNative>>(
        'dartplant_fixture_p6_abi_probe',
      )
      .asFunction();
  static final _ForcedStackClosureProbeDart _forcedStackClosureProbe = _library
      .lookup<NativeFunction<_ForcedStackClosureProbeNative>>(
        'dartplant_fixture_forced_stack_closure_probe',
      )
      .asFunction();
  static final _TypeArgumentsProofPrepareDart _typeArgumentsProofPrepare =
      _library
          .lookup<NativeFunction<_TypeArgumentsProofPrepareNative>>(
            'dartplant_fixture_type_arguments_proof_prepare',
          )
          .asFunction();
  static final _TypeArgumentsProofDart _typeArgumentsProof = _library
      .lookup<NativeFunction<_TypeArgumentsProofNative>>(
        'dartplant_fixture_type_arguments_proof',
      )
      .asFunction();
  static final _ExceptionBridgeLifetimeInstallDart
      _exceptionBridgeLifetimeInstall = _library
          .lookup<NativeFunction<_ExceptionBridgeLifetimeInstallNative>>(
            'dartplant_fixture_exception_bridge_lifetime_install',
          )
          .asFunction();
  static final _ExceptionBridgeLifetimeProbeDart _exceptionBridgeLifetimeProbe =
      _library
          .lookup<NativeFunction<_ExceptionBridgeLifetimeProbeNative>>(
            'dartplant_fixture_exception_bridge_lifetime_probe',
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
  static final _ResetBoolSemanticProbeDart _resetBoolSemanticProbe = _library
      .lookup<NativeFunction<_ResetBoolSemanticProbeNative>>(
        'dartplant_fixture_reset_bool_semantic_probe',
      )
      .asFunction();
  static final _BoolSemanticProbeDart _boolSemanticProbe = _library
      .lookup<NativeFunction<_BoolSemanticProbeNative>>(
        'dartplant_fixture_bool_semantic_probe',
      )
      .asFunction();
  static final _ResetVerifiedAbiDoubleProbeDart _resetVerifiedAbiDoubleProbe =
      _library
          .lookup<NativeFunction<_ResetVerifiedAbiDoubleProbeNative>>(
            'dartplant_fixture_reset_verified_abi_double_probe',
          )
          .asFunction();
  static final _VerifiedAbiDoubleProbeDart _verifiedAbiDoubleProbe = _library
      .lookup<NativeFunction<_VerifiedAbiDoubleProbeNative>>(
        'dartplant_fixture_verified_abi_double_probe',
      )
      .asFunction();
  static final _MarkVerifiedAbiDoubleSharedDart _markVerifiedAbiDoubleShared =
      _library
          .lookup<NativeFunction<_MarkVerifiedAbiDoubleSharedNative>>(
            'dartplant_fixture_mark_verified_abi_double_shared',
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

  static int simpleFacadeInstall() => _simpleFacadeInstall();

  static int simpleFacadeStage1() => _simpleFacadeStage1();

  static int simpleFacadeStage2() => _simpleFacadeStage2();

  static int enableAdvancedOrdinaryHook() => _enableAdvancedOrdinaryHook();

  static int enableForcedStackClosureHook() => _enableForcedStackClosureHook();

  static int p6AbiInstall() => _p6AbiInstall();

  static int p6AbiProbe() => _p6AbiProbe();

  static int forcedStackClosureProbe() => _forcedStackClosureProbe();

  static int typeArgumentsProofPrepare(Object closure) =>
      _typeArgumentsProofPrepare(closure);

  static int typeArgumentsProof() => _typeArgumentsProof();

  static int exceptionBridgeLifetimeInstall() =>
      _exceptionBridgeLifetimeInstall();

  static int exceptionBridgeLifetimeProbe() => _exceptionBridgeLifetimeProbe();

  static void resetNullSemanticProbe() => _resetNullSemanticProbe();

  static int nullSemanticProbe() => _nullSemanticProbe();

  static void resetBoolSemanticProbe() => _resetBoolSemanticProbe();

  static int boolSemanticProbe() => _boolSemanticProbe();

  static void resetVerifiedAbiDoubleProbe() => _resetVerifiedAbiDoubleProbe();

  static int verifiedAbiDoubleProbe() => _verifiedAbiDoubleProbe();

  static int markVerifiedAbiDoubleShared() => _markVerifiedAbiDoubleShared();
}
