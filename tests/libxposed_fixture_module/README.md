# External libxposed integration fixture

This APK targets only `dev.dartplant.dartplant_fixture` and declares libxposed API 102.
It lives in the DartPlant repository; Vector/LSPosed source trees are references,
not copied dependencies. Java owns package/hot-reload admission. Native API v2
provides a stable callback and local publication gate on native ARM64.

On a translated x86_64 AVD, Java lifecycle injection is real, but Vector's
x86_64 Dobby and do_dlopen hook cannot patch/observe ARM64 guest code.
The module explicitly selects DartPlant's strict ARM64 Dobby backend there;
that result is **not** a native-v2 Hook test.

An optional native ARM64 test build can opt into
`-PdartplantNativeStrictFallback=true`. It runs the framework's native-v2
two-hook/two-unhook preflight first; only if that fails does it explicitly
select DartPlant's local strict-Dobby backend. The runner must use
`--expect-backend native-strict-dobby`, which checks the logged failure and
the explicit backend selection. This does **not** satisfy the Native API v2
production gate; the default package keeps this fallback disabled.

The Flutter fixture has a separate `DARTPLANT_EXTERNAL_MODULE=1` build mode.
Its built-in DartPlant runtime remains uninitialized in this mode. An FFI call
on the Dart mutator supplies the exact VM registers and Dart API DL data to
the injected module. The module owns one `dartplant_core`, a single fixed
VM adapter, and the physical HookRecords within one `libdartplant.so`.
One fixed Dart family contains two exact, source-verified machine descriptors:
PRODUCT (release) and non-product (profile). The real Flutter snapshot feature
set selects one row before VM observation or Hook installation; the module
never falls back to another Dart family just because an ABI fingerprint looks
compatible. The generic adapter's original three-descriptor ABI is unchanged.

## Local AVD checks

Build the module with `./gradlew :app:assembleRelease
-PdartplantFamily=3.12.1`, then verify the APK with
`scripts/ci/check_libxposed_module.py`. The checker requires the selected
family's dedicated ELF section, a STORED ARM64 library, Native API entry,
fixture-only scope, required exports and `NODELETE`.

For the external Flutter release fixture, set `DARTPLANT_EXTERNAL_MODULE=1`
while running `scripts/ci/build_flutter_fixture.py`; supply its APK and the
matching module APK to `scripts/ci/run_libxposed_fixture.py`. Set
`--expect-backend translated-dobby` on the AVD. Use
`--expect-family-mismatch` for a deliberate wrong-family package; a clean
rejection must leave Java interception working and install no Dart Hook.
Repeat the matching-family run against the external `profile` APK: release
and profile have different VM transition/profile rows even when they share
the same snapshot hash. A release-only fixed adapter is not a complete
family package.

Positive evidence must show Java value 107, exact owner-thread bootstrap,
Dart Hook enter/leave **and actual Smi result rewriting**:
`instrumentedAdd(2, 3)` must return 115 while A is hooked (original Smi
result 5 -> 115), return 5 after unhook, and return 115 after
rebind; B/B2 retain their unmodified result 5 because their logical owners
are distinct. Mere callback counts with an unchanged result do not satisfy
the external module's behavior gate. Retirement requires zero rewrite failures.
Argument rewriting is a separate ABI gate: the observed legacy x1/x2
locations do not currently decode to the expected Smi arguments in this
external AOT path. Do not write into unverified locations or claim that this
fixture tests argument rewriting.
The independent external null/bool hooks must match the built-in fixture's
semantic results: a canonical null passthrough, a second object call overridden
to null, and two bool results flipped back to their runtime-provided inputs.
The module's native `semantic_probe` requires exactly two null enter/leave
callbacks, one null override, one observed true, one observed false, and zero
semantic failures. After B/B2 are destroyed, A also runs allocation pressure
on its own Dart mutator and proves `115` with correct enter/leave pairing on
both sides. **This is not a moving-GC object-root relocation proof.**
The separate `externalObjectRootProbe` DOES exercise a strong VM object root:
its exact AOT/compiler sidecar proves the `DartCallLayout` required for the
GC-safe generated/native bridge. The first result is retained by the injected
module, the Dart caller drops its object reference and runs GC pressure, and
the next callback reads the live root and releases it. The AVD run observed
`alive=1` and `relocated=1` (a changed raw object address). This is separate
from the original `generic_gc`, which roots TypeArguments and its elements
during the same invocation.
The retained `externalThrowingProbe` Hook independently exercises a real
Dart non-local unwind: one enter, no leave, one exception callback, immediate
caller catch result 2, and an idle physical record. It does not alter the
unannotated optimized `verifiedAbiThrowingStack` P6 target. Without an owner-bound
V4 object bridge the callback must fail closed when asked for exception and
stacktrace objects. This does not replace the embedded P6 exception bridge
lifetime/object-read tests.

The current external runner reports `dobby_parity_complete=false`. It
accepts this narrower subset only when every implemented external scenario
and its native evidence pass; it must never describe the whole embedded
Dobby corpus as passed. Pending independent external coverage includes
the original Function's verified argument rewrite, TypeArguments generated
root relocation, closure/generic and P6 ABI corpus, full exception bridge
lifetime/object access, and deferred image refresh.

Build the external Flutter fixture **before** its module APK: the fixture
builder emits `external_root_sidecar.h` after the exact `libapp.so` and
compiler oracle are available. The module refuses to compile with a placeholder.
These sidecars are artifact/mode-specific, although the fixed VM-family
adapter exposes both PRODUCT and profile descriptors. Match each module to
the same exact fixture build, not just the Dart version.
The controlled RX mapping test must independently show an
unmapped old page, a different file inode, and a new image at the same VA,
then reconcile an independent `RuntimeImageSet` copy: preserve the logical id
but advance its physical incarnation epoch so the old owner receipt cannot
be treated as the new owner. The detached test set is not the actual Flutter
runtime's active image set.

## Strict boundaries

- The RX memfd test validates physical mapping primitives, **not** Flutter
  `libapp.so` unloading or runtime owner-tree remapping.
- The existing deferred fixture sends a logical unload notification while the
  Flutter mapping is retained; it is not a `dlclose` proof.
- Hot reload is refused until the still-registered native v2 callback, module
  generations, Hook drain and VM root leases have a complete teardown contract.
- Native-v2 ownership must be separately validated on a real ARM64 host.
- Family-specific modules must be built using the corresponding exact Dart
  SDK source checkout in CI. A local compilation against a different source
  checkout is not evidence of exact source-version compatibility.
