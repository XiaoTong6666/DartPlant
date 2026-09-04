# DartPlant

DartPlant provides verified Dart AOT method-to-address resolution and native
instrumentation for Android arm64 Flutter applications.

The core is developed and verified through standalone host backends, including
the Dobby-backed Android fixture. Optional loader adapters are exercised
separately from the runtime core.

> Retained Dart `Function` objects are resolved metadata-free from the loaded
> snapshot plus a validated `LiveVmContext`. PRODUCT AOT can deliberately drop a
> logical `Function` object while retaining its `Code`; that deleted identity
> cannot be reconstructed exactly from the runtime heap. Those methods may use
> a compiler-generated embedded `DartPlantArtifactBundle`, bound to the exact
> snapshot hash, `libapp.so` build-id and final code fingerprint. The bundle
> self-registers and is consumed automatically; unbound legacy metadata is never
> used as a runtime fallback.

Repository responsibilities:

```text
darthook
    DartPlant native core, runtime API, resolver, and ARM64 tests

scripts
    build, test, format, lint, audit, and device-test orchestration for DartPlant
```

DartPlant is consumed as a third-party CMake dependency. The consuming Android
or native project owns its Gradle/module packaging and selects a host backend in
the same way it selects another native dependency such as Dobby:

```cmake
set(DARTPLANT_BUILD_DOBBY_ADAPTER ON)
add_subdirectory(third_party/dartplant)
target_link_libraries(consumer_native PRIVATE dartplant_core dartplant_adapter_dobby)
```

The optional adapter targets are `dartplant_adapter_dobby`,
`dartplant_adapter_shadowhook`, `dartplant_adapter_flutter_vm`, and the
version-scoped `dartplant_adapter_flutter_vm_3_4_4`. The Flutter VM adapters
require `DARTPLANT_DART_SDK_ROOT`. The generic target exposes the checked-in
descriptor matrix, while the `_3_4_4` target compiles exactly one Flutter
3.22.3 / Dart 3.4.4 PRODUCT ARM64 descriptor and is the target used by the
release fixture. The `native_init` compatibility module for
LSPosed Native API integration is built with `DARTPLANT_BUILD_ANDROID_MODULE=ON`.
None of these adapters is linked into `dartplant_core`, and DartPlant does not
own the consumer's module lifecycle.

The core host backend contract is adapter-independent and can bind a backend
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

Adapters that can prepare an original trampoline before patch publication should
also provide `hook_with_publication`. It must call
`DartPlantHostHookTransaction::backup_ready` before publishing the target. The
reserved `DARTPLANT_HOST_HOOK_FAILED_AFTER_PUBLISHED` result tells DartPlant to
retain its executable callback veneer even after the adapter restores the
target; unknown non-zero results retain the legacy never-published behavior.

Real Dart control-flow hooks additionally pass through a DartPlant-owned
publication gate. Strict hosts publish their backup before the backend can make
that gate reachable. The built-in LSPosed/Vector `native_init` adapter uses an
internal audited-local-gate policy instead: their unchanged synchronous Native
API v2 `hook(target, replacement, &backup)` may make the gate reachable before
returning, but CPUs that arrive early remain inside the gate until DartPlant has
the callable backup and a fully initialized HookRecord. During unhook the gate
moves through `ARMED -> DRAINING -> CLOSED -> BYPASS_TARGET`: DRAINING calls
still enter the dispatcher as listener-free tracked passthrough invocations so
recursive Dart calls cannot deadlock, while CLOSED is reached atomically only
after both HookRecord invocations and gate handoff pins are idle. No stale gate
fetch can therefore use a backend trampoline after physical unhook. Arbitrary public legacy HostApi
providers are still conservative/raw-only unless they expose the strict
publication extension; DartPlant does not infer local-gate safety from a plain
function-pointer shape.

The current implementation provides:

- A stable C ABI: existing public struct prefixes remain byte-for-byte stable,
  and versioned extensions are appended behind `struct_size` gates.
- A generic `DartPlantHostApi` hook-backend contract, with `native_init`
  compatibility retained as an optional loader adapter.
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
- Entry-owned ARM64 return interception: RET patches are discovered from the
  selected entry's reachable control-flow graph, and the return dispatcher also
  verifies the real Dart caller LR before consuming a TLS invocation frame.
- Artifact/ABI fingerprints normalize only exact byte patches currently owned by
  DartPlant (entry backend + RET interception), in reverse installation order;
  unrelated or third-party code mutations remain visible and fail closed.
- Limited argument/result mutation, skip-original, and original-call policy.
- Per-thread invocation depth and nested call tracking.
- Shared HookChain listeners with priority ordering and safe removal snapshots.
- Host unit tests and standalone ARM64 Android Dobby integration tests.
- A `native_init` lifecycle adapter with already-loaded image rescan,
  dependency-load refresh, module identity selection, and runtime-generation
  fail-closed invalidation.
- An internal ABI evidence contract/solver and SDK-aligned ARM64 Dart calling
  convention allocator, with exact evidence binding to snapshot hash, app
  build-id, logical Function identity, runtime generation, and Code fingerprint.
- An experimental offline AOT ABI evidence analyzer plus a compiler-oracle
  exporter for matching transformed DILL `vm.unboxing-info.metadata`.

The provided loader adapter can receive module callbacks while its host still
holds loader-side synchronization. The callback therefore only increments an
atomic refresh epoch and wakes a process-lifetime worker. ELF enumeration,
snapshot discovery, runtime locking, hook invalidation, and host `unhook_func`
calls all run on that worker after the callback returns. Every successful loader
callback schedules a full scan; events are coalesced by epoch rather than
filtered by the top-level `dlopen` name.
The worker and callback code are process-lifetime state; the Android module is
linked `NODELETE` because the host ABI has no callback-unregister operation.
This release supports process-lifetime Flutter app/runtime images. Hosts should
call `dartplant_runtime_on_module_unloading()` before `dlclose`, then
`dartplant_runtime_refresh_modules()` after loader changes. Arbitrary concurrent
`dlclose` without that ordering remains unsupported. Deferred loading units and
multiple independent Flutter engine image sets are refreshed conservatively but
are not exposed as separate public snapshot-index namespaces.

Normal consumers use DartPlant's high-level public API. DartPlant owns runtime
creation, module refresh, Live VM bootstrap, entry/payload target sharing, and matching
embedded compiler artifacts:

```cpp
#include <dartplant/adapters/dobby.h>
#include <dartplant/dartplant.h>
#include <dartplant/hook.h>
#include <dartplant/invocation.h>

DartPlantInitInfo init = {
    .struct_size = sizeof(DartPlantInitInfo),
    .version = DARTPLANT_INIT_API_VERSION,
    .host_api = dartplant_dobby_host_api(),
};
dartplant_init(&init);

DartPlantMethodQuery query = {
    .struct_size = sizeof(DartPlantMethodQuery),
    .library_uri = "package:app/main.dart",
    .class_name = "Calculator",
    .function_name = "add",
    .entry_kind = DARTPLANT_ENTRY_DEFAULT,
};

DartPlantMethod* method = nullptr;
dartplant_find_method(&query, &method);

DartPlantHookOptions options = {
    .struct_size = sizeof(DartPlantHookOptions),
    .on_enter = on_enter,
    .on_leave = on_leave,
};
DartPlantHookHandle* hook = nullptr;
dartplant_hook_method(method, &options, &hook);
```

Callbacks read/write logical values rather than ARM64 locations:

```cpp
void on_enter(DartPlantInvocation* invocation, void*) {
    DartPlantValue value{};
    if (dartplant_invocation_get_argument(invocation, 0, &value) == DARTPLANT_OK) {
        // inspect or replace value...
        dartplant_invocation_set_argument(invocation, 0, &value);
    }
}
```

`dartplant_invocation_call_original()` and
`dartplant_invocation_skip_original()` control the original call. Raw GP/FP
register access remains available as an escape hatch when a method ABI cannot
be proven; semantic argument/result APIs then fail closed with
`DARTPLANT_UNSUPPORTED_ABI` instead of guessing a layout.

The internal typed path is separated into:

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
return bridge captures `x0+x1+v0` before the leave callback.

Compiler-generated sidecars emit one `DartPlantArtifactBundle`. Including the
generated C++ header self-registers that bundle at module load. Method lookup
automatically binds its exact snapshot index after a live-index miss and
automatically attaches matching compiler ABI evidence before the logical hook
is installed. Normal consumers do not call snapshot-index or ABI-evidence
registration APIs.

The retained `FunctionType` is used only for semantic/formal-shape validation;
machine representation is never inferred from Dart source types. Exact evidence
is additionally bound to snapshot hash, `libapp.so` build-id, logical Function
identity, entry VA, code size, Code identity proof, and final Code bytes. Shared
physical entry targets deliberately suppress the typed overlay because a
physical entry cannot prove which logical alias reached it.

This remains a native C/C++ callback API. A sidecar is optional: retained
Functions can still be resolved metadata-free and raw instrumentation remains
available without typed ABI evidence. Retained closure Functions map supplied
optional positional/named formals through the live ArgumentsDescriptor and expose
the verified generic TypeArguments vector as one opaque tagged object. With an
exact VM V3 adapter, individual TypeArguments elements are captured before the
Generated->Native safepoint and placed in the same VM-visible root lease, so
element reads remain valid across moving GC without exposing arbitrary VM
memory. Dropped optional closures still require richer artifact evidence;
arbitrary Dart heap-object construction and synchronous Dart callbacks remain
fail-closed.

Public logical-hook lifecycle:

```text
dartplant_hook_method()
    -> returns one DartPlantHookHandle subscription
    -> internally creates/reuses the DartEntryTarget physical hook

dartplant_unhook_handle()
    -> removes only this logical subscription
    -> final subscription removes the physical trampoline

dartplant_hook_handle_is_idle()
    -> becomes true after in-flight callback snapshots drain
```

Advanced/runtime tooling remains available through `dartplant/runtime.h` and
`dartplant/advanced/*` for diagnostics, fixture construction, exact VM layout
research, and compatibility. `live_vm.h`, `abi_evidence.h`, and
`flutter_snapshot.h` at their historical paths are compatibility forwarding
headers; new tooling should include the corresponding `dartplant/advanced/*`
header directly. The legacy RuntimeProfile/manual mapping API is not part of the
normal consumer workflow.

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
    -> Function* -> Code* -> DartCodePayload -> DartEntryTarget
    -> if the logical Function was dropped by PRODUCT AOT:
       exact compiler/artifact SnapshotIndex -> validated DartEntryTarget
    -> executable-range validation
    -> selected host backend hook(target_entry, replacement, backup)
```

Private Dart VM layout facts are no longer duplicated across the live resolver
and exception bridge. `scripts/data/dart_vm_profiles.json` is the checked-in
manifest for exact PRODUCT/ARM64/compressed profiles; `scripts/main.py profiles`
generates `src/vm/generated/runtime_profiles.generated.h`. One profile record
binds the live heap layout, canonical bool layout, FunctionType layout, Dart
fixed registers, Dart GP/FPU argument register sequences, and the Thread
`JumpToFrame` cached-entry offset to the same snapshot hash. It also binds the
ARM64 AOT monomorphic/polymorphic instruction-entry offsets needed to reproduce
Dart's own `Code::PayloadStartOf()` calculation instead of guessing a Code
payload boundary from the minimum cached entry address. The generator can
also verify the common ABI contract directly against a Dart SDK source checkout:

```bash
python3 scripts/main.py profiles --check --sdk-root ../sdk
```

That source check currently verifies the ARM64 `THR/PP/CODE_REG/HEAP_BITS/
NULL_REG/SPREG/ARGS_DESC_REG` assignments, the `R1,R2,R3,R5,R6,R7` and
`V0..V5` Dart argument sequences, independent GP/FPU allocation in
`ComputeCallingConvention()`, the register-parameter policy in
`Function::MaxNumberOfParametersInRegisters()`, and Thread's cached
`JumpToFrame` entry. It additionally verifies the precompiled
`Code::HasMonomorphicEntry()` / `Code::PayloadStartOf()` relationship and that
`Code.instructions_length_` is measured from that payload start. It resolves
every checked-in profile's exact Dart SDK tag and verifies every manifest field
that the SDK emits as a PRODUCT + ARM64 + compressed `AOT_*` offset, so those
source-verifiable private layout facts cannot silently drift from the generated
table.

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
The optional `dartplant_adapter_flutter_vm` target owns the exact private VM
transition contract separately from the fixture. Its public opaque owner and
immutable descriptors enumerate snapshot hash, Flutter build-id, Dart/Flutter
versions, architecture and compression mode. Unknown fingerprints fail closed.
`dartplant_adapter_flutter_vm_3_4_4` is a separate product target built from the
same implementation but compile-gated to exactly one descriptor: Flutter 3.22.3
/ Dart 3.4.4 Android ARM64 PRODUCT AOT. The release fixture links that target and
asserts `descriptor_count == 1` at cold start. The 3.5.0 / 3.12.1 descriptors in
the generic target are compatibility profiles, not device-cold validation claims.
The optional `native_init` adapter owns loader-facing lifecycle only: it validates and
stores the host entries, refreshes the process module inventory on initialization
and every module-loaded callback, and lets each runtime instance select its own
app/runtime image incarnation. The core resolver remains independent of
host-specific Java/Kotlin APIs.

The ARM64 device regression currently proves the public runtime method hook API
with a standalone Dobby host backend, including enter, original call, leave,
result mutation, skip-original, unhook, and executable-range validation.

The target-specific metadata generator in `scripts/metadata.py` remains legacy
offline tooling. Exact dropped-Function sidecars are a separate compiler-oracle
path: they are optional per target, self-register as an embedded
`DartPlantArtifactBundle`, and cannot replace a missing exact identity with a
heuristic name.

The compiler sidecar path now accepts DartPlant's host Capstone analyzer as an
independent machine-code structural cross-check. Compiler
`vm.unboxing-info.metadata` remains the ABI source of truth; optimized CFG text
and actual final `libapp.so` machine-code observations may confirm that truth or
fail closed on a concrete contradiction, but an `Unknown` structural result
never fabricates an ABI fact. The analyzer publishes CFG/basic-block facts,
incoming GP/FPU/x15-stack provenance, return sites, direct call-site/target
edges, external branch edges, distinct indirect-call
and indirect-branch sites, and ArgumentsDescriptor usage for later
entry-kind/closure evidence work.

The exposed invocation frame always preserves raw ARM64 machine truth. When
exact compiler evidence and Code identity prove a `DartCallLayout`, the same
frame additionally exposes semantic argument/result values. Unknown layouts
remain raw rather than being guessed. Object retention still requires an
attached VM adapter and the correct isolate scope. The exact Flutter VM V3
adapter publishes VM-visible persistent roots, performs generated/native
safepoint transitions, and keeps the root lease authoritative while moving GC
can make saved registers stale. A verified closure receiver and generic
TypeArguments vector can be inspected during enter; generic TypeArguments
elements are read only from pre-safepoint rooted slots, never by dereferencing a
possibly relocated TypeArguments object from native state.

Exception handling is intentionally asymmetric: `on_leave` remains normal-return
only, while `dartplant_hook_handle_set_exception_callback()` provides a read-only
notification when JumpToFrame proves that an exception unwound out of the hooked
frame. During that callback, exact VM V3 adapters expose the current exception and
stacktrace as raw tagged values through `dartplant_invocation_get_exception()` and
`dartplant_invocation_get_stacktrace()`. Retention, suppression and replacement
are not exposed.

Runtime refresh is incarnation-aware. Hosts can call
`dartplant_runtime_on_module_unloading()` before `dlclose` to invalidate hooks
while code is mapped, then `dartplant_runtime_refresh_modules()` after loader
changes. A changed app/runtime/snapshot identity advances generation and requires
fresh bootstrap. Deferred loading units and simultaneous independent Flutter
engine instances remain outside the current single-app-image resolver model.

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
python3 scripts/main.py profiles --check --sdk-root ../sdk
python3 scripts/main.py build host
python3 scripts/main.py test host
python3 scripts/main.py metadata ./target.apk -o ./build/target.metadata.json
python3 scripts/main.py build android
python3 scripts/main.py test device --device <serial>
```

Hosts loaded by the Android/system linker may use native C++ `thread_local`.
For custom/minimal loaders that do not implement ELF dynamic TLS, configure
`DARTPLANT_REQUIRE_MINIMAL_LINKER_COMPAT=ON`; DartPlant then switches its
per-thread error/callback state to `pthread_key_t` storage and post-build audits
the final shared object for forbidden AArch64 TLS relocations. The same policy
can be run explicitly against any final host/module ELF:

```bash
python3 scripts/main.py loader-audit ./libmodule.so
```

The audit is intentionally applied to the final shared object rather than a
static DartPlant archive because custom-loader compatibility is a property of
the complete host image.

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

This standalone device path does not build or deploy the optional `native_init`
module. Build the `dartplant` shared target separately with
`-DDARTPLANT_BUILD_ANDROID_MODULE=ON` when testing loader-host integration.

The runtime-layer files are:

```text
include/dartplant/runtime.h
include/dartplant/runtime_profile.h
include/dartplant/invocation.h
src/runtime/dart_runtime_resolver.cpp
src/runtime/dart_dispatch_hook.cpp
src/runtime/dart_invocation.cpp
```
