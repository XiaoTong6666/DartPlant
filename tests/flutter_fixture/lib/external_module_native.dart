import 'dart:ffi';

import 'package:flutter/services.dart';

const externalModuleChannel = MethodChannel('dev.dartplant.fixture/launch');

final class ExternalModuleNative {
  ExternalModuleNative._();

  static final _library =
      DynamicLibrary.open('libdartplant_external_bridge.so');
  static final _register = _library
      .lookup<NativeFunction<Void Function(Uint64)>>(
        'dartplant_external_bridge_register',
      )
      .asFunction<void Function(int)>();
  static final _bootstrap = _library
      .lookup<NativeFunction<Uint64 Function(Uint64)>>(
        'dartplant_external_bridge_bootstrap',
      )
      .asFunction<int Function(int)>();

  static int bootstrap(int callback) {
    _register(callback);
    return _bootstrap(NativeApi.initializeApiDLData.address);
  }

  static int counters(int address) =>
      Pointer<NativeFunction<Uint64 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int retire(int address) =>
      Pointer<NativeFunction<Uint64 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int mapping(int address) =>
      Pointer<NativeFunction<Uint64 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int typeArgsPrepare(
    int address,
    Object retainedClosure,
    Object pressureSendPort, {
    required bool requireRelocation,
  }) =>
      Pointer<
          NativeFunction<Int32 Function(Handle, Handle, Uint8)>>.fromAddress(
        address,
      ).asFunction<int Function(Object, Object, int)>()(
        retainedClosure,
        pressureSendPort,
        requireRelocation ? 1 : 0,
      );

  static int typeArgsProbe(int address) =>
      Pointer<NativeFunction<Uint64 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int p6Install(int address) =>
      Pointer<NativeFunction<Int32 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int p6Probe(int address) =>
      Pointer<NativeFunction<Uint64 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int closureInstall(int address) =>
      Pointer<NativeFunction<Int32 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int closureProbe(int address) =>
      Pointer<NativeFunction<Uint64 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int ordinaryInstall(int address) =>
      Pointer<NativeFunction<Int32 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int ordinaryMarkShared(int address) =>
      Pointer<NativeFunction<Uint64 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int ordinaryProbe(int address) =>
      Pointer<NativeFunction<Uint64 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int exception(int address) =>
      Pointer<NativeFunction<Uint64 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();

  static int objectRoot(int address) =>
      Pointer<NativeFunction<Uint64 Function()>>.fromAddress(
        address,
      ).asFunction<int Function()>()();
}
