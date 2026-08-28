# DartPlant

DartPlant provides verified Dart AOT method-to-address resolution and native
instrumentation for Android arm64 Flutter applications.

The core is developed and verified independently from LSPosed. Standalone host
backends, including the Dobby-backed Android fixture, validate the runtime
before the optional LSPosed Native API adapter is exercised.

> Retained Dart `Function` objects are resolved metadata-free from the loaded
> snapshot plus a validated `LiveVmContext`. PRODUCT AOT can deliberately drop a
> logical `Function` object while retaining its `Code`; that deleted identity
> cannot be reconstructed exactly from the runtime heap. Those methods may use
> an explicit compiler/artifact snapshot index bound to the exact snapshot hash,
> `libapp.so` build-id and final code fingerprint. Unbound legacy metadata is
> never used as a `DartPlantRuntime` fallback.

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

The core host backend contract is independent of LSPosed and can bind a backend
instance through `user_data`:

```cpp
DartPlantHostApi host = {
    .struct_size = sizeof(DartPlantHostApi),
    .version = DARTPLANT_HOST_API_VERSION,
    .user_data = backend,
    .hook = host_hook,
    .unhook = host_unhook,
};
dartplant_install_host_api(&host);
```

Each installed physical hook retains the exact backend binding that created it,
so replacing the process default backend does not redirect a later unhook.

The current implementation provides:

- A stable C ABI.
- A generic `DartPlantHostApi` hook-backend contract, with the established
  LSPosed `native_init` ABI retained as a compatibility adapter.
- ELF module enumeration and load-bias aware address resolution.
- Executable segment, build-id, and code fingerprint validation.
- Metadata-free live VM `Library/Class/Function -> Code` resolution for retained
  `Function` objects.
- Optional exact artifact snapshot-index fallback for logical Functions removed
  by PRODUCT AOT, with snapshot/build-id/fingerprint validation.
- Legacy/offline JSON metadata parsing retained outside the production runtime
  resolver.
- Raw method/address hooks with original trampolines and unhook support.
- ARM64 native enter/leave callbacks with a validated GP-register profile.
- Limited argument/result mutation, skip-original, and original-call policy.
- Per-thread invocation depth and nested call tracking.
- Shared HookChain listeners with priority ordering and safe removal snapshots.
- Host unit tests and standalone ARM64 Android Dobby integration tests.
- LSPosed `native_init` lifecycle adapter with already-loaded image rescan,
  dependency-load refresh, module identity selection, and runtime-generation
  fail-closed invalidation.
- An internal ABI evidence contract/solver and SDK-aligned ARM64 Dart calling
  convention allocator, with exact evidence binding to snapshot hash, app
  build-id, logical Function identity, runtime generation, and Code fingerprint.
- An experimental offline AOT ABI evidence analyzer plus a compiler-oracle
  exporter for matching transformed DILL `vm.unboxing-info.metadata`.

LSPosed invokes native module callbacks while holding its module-registry mutex.
The adapter callback therefore only increments an atomic refresh epoch and wakes
a process-lifetime worker. ELF enumeration, snapshot discovery, runtime locking,
hook invalidation, and host `unhook_func` calls all run on that worker after the
LSPosed callback returns. Every successful loader callback schedules a full scan;
events are coalesced by epoch rather than filtered by the top-level `dlopen` name.
The worker and callback code are process-lifetime state; the Android module is
linked `NODELETE` because the host ABI has no callback-unregister operation.
This release supports process-lifetime Flutter app/runtime images. Arbitrary
concurrent `dlclose` of an image with installed hooks is unsupported until a
host supplies a pre-unload synchronization callback or a backend-safe retire API.

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

The conservative public runtime profile supports raw method hooks and raw
callback instrumentation by default. Callback installation itself does not
require a guessed argument layout: `dartplant_invocation_get_gp_register()` /
`dartplant_invocation_get_fp_register()` remain available while semantic
argument/result APIs fail closed until a layout is proven.
Its `argument_locations[]` / `result_location` fields are now the legacy/manual
raw-mapping escape hatch; they are retained for compatibility and ABI research,
not as the final typed-call design. Callers that explicitly use that legacy
typed/raw-word mapping must still provide a validated register mapping:

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

The new internal typed path is separated into:

```text
compiler/AOT evidence
    -> DartAbiRepresentation
    -> DartFunctionAbiResolution
    -> DartCallingConventionProfile
    -> DartCallLayout
    -> DartPlantInvocation verified overlay
```

For the validated ARM64 AOT convention, GP parameters allocate independently
from FP parameters (`x1,x2,x3,x5,x6,x7` vs `v0..v5`), overflow/forced-stack
parameters are normalized to Dart entry `SPREG/x15`, and returns cover `x0`,
`v0`, and the `x0+x1` pair channel. Verified `kUnboxedInt64` values are exposed
as `DARTPLANT_VALUE_INT64`, not as Smi or an untyped raw word. The original
return bridge now captures `x0+x1+v0` before the leave callback.

Exact compiler evidence can be registered after resolving the live method and
before installing its physical hook:

```cpp
const DartPlantAbiRepresentation parameters[] = {
    DARTPLANT_ABI_REPRESENTATION_UNBOXED_INT64,
    DARTPLANT_ABI_REPRESENTATION_UNBOXED_DOUBLE,
};
DartPlantCompilerAbiEvidence evidence = {
    .struct_size = sizeof(evidence),
    .snapshot_hash = sidecar.snapshot_hash,
    .app_build_id = sidecar.app_build_id,
    .code_fingerprint = sidecar.code_fingerprint,
    .parameter_representations = parameters,
    .parameter_count = 2,
    .result_representation = DARTPLANT_ABI_REPRESENTATION_UNBOXED_DOUBLE,
    .max_parameters_in_registers = 2,
};
dartplant_runtime_register_compiler_abi_evidence(runtime, method, &evidence);
dartplant_runtime_hook_method(runtime, method, &options, &hook);
```

Registration validates the retained `FunctionType` only for formal-slot shape;
it never derives representation from Dart source types. The evidence must also
match the current snapshot hash, `libapp.so` build-id and physical Code bytes.
`dartplant_runtime_get_method_abi_info()` reports whether the method ABI is
none, incomplete, verified, conflicting or unsupported, and callbacks can use
`dartplant_invocation_has_verified_abi()` to distinguish the typed overlay from
the always-available raw context. Shared `CodeTarget`s deliberately suppress
the typed overlay because a physical entry cannot prove which logical alias
reached it.

This remains a native C/C++ callback API. Evidence loading/sidecar policy is
owned by the consuming host; DartPlant does not make an ABI sidecar a production
runtime prerequisite. Optional-argument transport, closures, arbitrary Dart
heap-object construction, and synchronous Dart callbacks remain fail-closed.

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

Offline tooling still records the snapshot-instructions offset used by the SDK
analyzer (`code_section_va + code_offset`) for analysis and compatibility.
`DartPlantRuntime` never consumes the legacy process-global metadata cache.
Instead, a consumer that needs an AOT-dropped logical Function may explicitly
register a `DartPlantSnapshotIndexInfo`. Retained methods always prefer the live
VM index; the artifact index is consulted only after an exact live miss and is
validated against the current snapshot hash, module build-id, executable range,
and final machine-code fingerprint.

## Current AOT status

The reusable production resolver is hybrid because Dart PRODUCT AOT has two
different identity-retention cases:

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
    -> if the logical Function was dropped by PRODUCT AOT:
       exact compiler/artifact SnapshotIndex -> validated CodeTarget
    -> executable-range validation
    -> selected host backend hook(target_entry, replacement, backup)
```

The Flutter ARM64 release fixture packages no raw DartPlant metadata asset. It
validates both the live path and a generated exact sidecar for one deliberately
dropped ordinary AOT Function. The sidecar producer re-runs the matching
deterministic `gen_snapshot` for compiler diagnostics, locates the emitted
machine bytes uniquely in the final `libapp.so`, and binds the record to that
artifact's snapshot hash/build-id/fingerprint. When initialization is entered from Dart FFI, the
fixture captures Dart's reserved ARM64 registers before a C++ prologue can reuse
them and validates that context with the same live-VM semantic checker. The
process-wide signal sampler remains a fallback for hosts without such a mutator
entry. Current production limitations include raw-layout profile coverage, ABI
scope, and exact identity availability for AOT-dropped Functions; unknown or
heuristic identities fail closed.
The optional LSPosed adapter owns loader-facing lifecycle only: it validates and
stores the host entries, refreshes the process module inventory on initialization
and every module-loaded callback, and lets each runtime instance select its own
app/runtime image incarnation. The core resolver remains independent of LSPosed's
Java/Kotlin APIs.

The ARM64 device regression currently proves the public runtime method hook API
with a standalone Dobby host backend, including enter, original call, leave,
result mutation, skip-original, unhook, and executable-range validation.

The target-specific metadata generator in `scripts/metadata.py` remains legacy
offline tooling. Exact dropped-Function sidecars are a separate compiler-oracle
path: they are optional per target, explicitly registered, and cannot replace a
missing exact identity with a heuristic name.

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
