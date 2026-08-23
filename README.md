# DartPlant

DartPlant provides verified Dart AOT method-to-address resolution and native
instrumentation for Android arm64 Flutter applications.

The core is developed and verified independently from LSPosed. Standalone host
backends, including the Dobby-backed Android fixture, validate the runtime
before the optional LSPosed Native API adapter is exercised.

> **Runtime metadata requirement: NONE.** Production Flutter AOT resolution is
> driven by the loaded snapshot identity plus a validated `LiveVmContext`.
> JSON metadata and snapshot indexes are retained only as legacy/offline analysis,
> compatibility, and test-oracle inputs.

Repository responsibilities:

```text
darthook
    DartPlant native core, runtime API, resolver, and ARM64 tests

scripts
    build, test, format, lint, audit, and device-test orchestration for DartPlant
```

DartPlant is consumed as a third-party CMake dependency. The consuming Android
or LSPosed project owns its Gradle/module packaging and selects a host backend
in the same way it selects another native dependency such as Dobby:

```cmake
add_subdirectory(third_party/dartplant)
target_link_libraries(consumer_native PRIVATE dartplant_core)
```

The optional LSPosed Native API entry point can be built as the provided Android
shared target or supplied by a consumer-owned module. LSPosed is not a core
runtime dependency, and DartPlant does not own the consumer's module lifecycle.

The current implementation provides:

- A stable C ABI.
- Pluggable host hook integration, with an optional LSPosed `native_init` adapter.
- ELF module enumeration and load-bias aware address resolution.
- Executable segment, build-id, and code fingerprint validation.
- Metadata-free live VM `Library/Class/Function -> Code` method resolution.
- Optional legacy/offline JSON metadata parsing and snapshot-index lookup.
- Raw method/address hooks with original trampolines and unhook support.
- ARM64 native enter/leave callbacks with a validated GP-register profile.
- Limited argument/result mutation, skip-original, and original-call policy.
- Per-thread invocation depth and nested call tracking.
- Shared HookChain listeners with priority ordering and safe removal snapshots.
- Host unit tests and standalone ARM64 Android Dobby integration tests.

The runtime API is intentionally separate from the legacy process-global API:

```cpp
DartPlantRuntimeProfile profile;
dartplant_runtime_profile_init_arm64_aot(&profile);

DartPlantRuntime* runtime = nullptr;
dartplant_runtime_create(&profile, &runtime);
dartplant_runtime_on_module_loaded(runtime, "libapp.so", module_handle);
dartplant_runtime_bootstrap_live_vm(runtime, nullptr, &bootstrap_info);
dartplant_runtime_find_method(runtime, &query, &method);
dartplant_runtime_hook_method(runtime, method, &options, &hook);
```

The conservative profile supports only raw method hooks. Native callbacks must
explicitly enable a validated register mapping:

```cpp
profile.flags = DARTPLANT_PROFILE_RAW_GP_ARGUMENTS |
                DARTPLANT_PROFILE_RAW_GP_RESULT;
profile.argument_count = 2;
profile.argument_locations[0] = {DARTPLANT_ABI_GP_REGISTER, 1, {0, 0}};
profile.argument_locations[1] = {DARTPLANT_ABI_GP_REGISTER, 2, {0, 0}};
profile.result_location = {DARTPLANT_ABI_GP_REGISTER, 0, {0, 0}};

DartPlantHookOptions options = {
    .struct_size = sizeof(options),
    .on_enter = on_enter,
    .on_leave = on_leave,
};
```

Inside `on_enter`, `dartplant_invocation_set_argument()` mutates mapped raw GP
arguments. `dartplant_invocation_set_result()` sets the return word and skips
the original call. `dartplant_invocation_call_original()` explicitly selects
the original path. Inside `on_leave`, result mutation changes x0 before returning
to the caller.

Raw GP mappings expose `DARTPLANT_VALUE_RAW_WORD` only. A profile may add
`DARTPLANT_PROFILE_TAGGED_GP_ARGUMENTS` and/or
`DARTPLANT_PROFILE_TAGGED_GP_RESULT` only when those locations are proven Dart
tagged values. Tagged locations expose Smi/heap-object semantics and canonical
null after the runtime has validated `NULL_REG`; unproven locations remain raw.

This is a native C/C++ callback API. It does not make arbitrary Dart heap
objects, closures, floating-point arguments, stack arguments, or synchronous
Dart callbacks safe.

HookChain listener lifecycle:

```text
runtime_hook_method()
    -> creates one HookRecord and first listener

runtime_add_listener()
    -> adds a priority-ordered listener to the same trampoline

remove_listener()
    -> marks the listener inactive; existing invocation snapshots finish

listener_is_idle()
    -> becomes true after all in-flight snapshots release the listener

release_listener()
    -> release only after remove + idle
```

Offline metadata still records the snapshot-instructions offset used by the SDK
analyzer (`code_section_va + code_offset`) for analysis, regression tests, and
compatibility with older callers. It is not required by the production runtime
resolver. Once `LiveVmContext` is available, the live VM is authoritative for
method identity, `Function*`, `Code*`, entry, code size, and shared-code aliases.

## Current AOT status

The reusable production runtime path is metadata-free:

```text
dl_iterate_phdr
    -> libapp.so ModuleImage
    -> _kDartIsolateSnapshotData / _kDartIsolateSnapshotInstructions
    -> snapshot hash/features + exact raw-layout profile
    -> LiveVmContext bootstrap (THR/PP/HEAP_BITS/NULL)
       -> preferred: validated Dart FFI-entry reserved registers
       -> fallback: process thread sampler
    -> IsolateGroup / ClassTable / ObjectStore / Library / Class / Function
    -> Function* -> Code* -> CodeTarget
    -> executable-range validation
    -> selected host backend hook(target_entry, replacement, backup)
```

The Flutter ARM64 release fixture packages no DartPlant metadata and validates
this path on a real device. When initialization is entered from Dart FFI, the
fixture captures Dart's reserved ARM64 registers before a C++ prologue can reuse
them and validates that context with the same live-VM semantic checker. The
process-wide signal sampler remains a fallback for hosts without such a mutator
entry. Current production limitations are profile coverage and ABI scope, not
metadata availability: live VM resolver v1 is currently tied to its validated
Dart/Flutter ARM64 AOT raw-layout profile, while other profiles fail closed.
LSPosed lifecycle integration remains a separate host concern.

The ARM64 device regression currently proves the public runtime method hook API
with a standalone Dobby host backend, including enter, original call, leave,
result mutation, skip-original, unhook, and executable-range validation.

The target-specific metadata generator in `scripts/metadata.py` is retained as
**offline tooling**. It invokes the vendored flutterdec adapter with the vendored
blutter backend and is useful for static analysis, compatibility tests, and
cross-checking snapshots. Its output is not a production runtime prerequisite.

The exposed invocation frame is deliberately limited to explicitly mapped ARM64
locations. Raw GP words remain opaque. Tagged GP profiles can decode Smi,
heap-object, and validated canonical-null values; object retention still requires
an attached VM adapter and the correct isolate scope.

## Host tests

```bash
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The copied project management scripts are the canonical entry point for
repeatable builds:

```bash
python3 scripts/main.py doctor
python3 scripts/main.py build host
python3 scripts/main.py test host
python3 scripts/main.py metadata ./target.apk -o ./build/target.metadata.json
python3 scripts/main.py build android
python3 scripts/main.py test device --device <serial>
```

The scripts under `scripts/` manage only DartPlant source builds and tests.

Clone the repository with its analysis submodules:

```bash
git submodule update --init --recursive
```

Metadata generation accepts an APK or a bare `libapp.so`. The vendored
`flutterdec-loader` extracts the four Dart snapshot images and resolves the
snapshot hash; the vendored adapter invokes `blutter.py` and the Rust indexer
writes the DartPlant method index:

```bash
python3 scripts/main.py metadata ./target.apk \
  -o ./build/target.metadata.json
```

If the APK marks its native entries as encrypted or uses a custom protected
container, extract `libapp.so` from the installed arm64 app image and pass it
explicitly:

```bash
python3 scripts/main.py metadata --libapp ./libapp.so \
  --libflutter ./libflutter.so \
  -o ./build/target.metadata.json
```

The indexer directly depends on the vendored submodules:

```text
third_party/flutterdec
    APK/ELF/snapshot loader, Dart profile, adapter and ProgramModel

third_party/blutter
    Dart snapshot deserialization and exact function/class/library recovery

third_party/capstone
    Capstone 5.0.3 source used to build the vendored blutter ARM64 analyzer

third_party/dobby
    XiaoTong6666/Dobby fork used by ARM64 Android device integration tests

tools/dartplant-indexer
    ProgramModel -> DartPlant metadata conversion only
```

## Android ARM64 build

```bash
cmake -S . -B build/android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-31 \
  -DDARTPLANT_BUILD_TESTS=OFF \
  -DDARTPLANT_BUILD_ANDROID_TESTS=ON \
  -DDARTPLANT_BUILD_ANDROID_MODULE=OFF
cmake --build build/android-arm64
```

Run on an attached ARM64 device:

```bash
python3 scripts/main.py test device --device <serial>
```

This standalone device path does not build or deploy the LSPosed module. Build
the optional `dartplant` shared target separately with
`-DDARTPLANT_BUILD_ANDROID_MODULE=ON` when testing LSPosed integration.

The runtime-layer files are:

```text
include/dartplant/runtime.h
include/dartplant/runtime_profile.h
include/dartplant/invocation.h
src/runtime/dart_runtime_resolver.cpp
src/runtime/dart_dispatch_hook.cpp
src/runtime/dart_invocation.cpp
```
