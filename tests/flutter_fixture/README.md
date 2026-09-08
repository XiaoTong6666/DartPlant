# dartplant_fixture

Cross-version Flutter ARM64 release fixture for DartPlant's source-verified VM
ABI families. CI currently builds it with Flutter 3.22.3 / Dart 3.4.4,
Flutter 3.24.0 / Dart 3.5.0, and Flutter 3.44.1 / Dart 3.12.1.

## Runtime contract

**Runtime metadata requirement: NONE.**

The fixture does not load or package DartPlant JSON metadata or a precomputed
SnapshotIndex. Startup discovers `libapp.so`, reads the Flutter snapshot
identity, bootstraps a validated `LiveVmContext`, and resolves Dart methods from
the live target VM.

The primary integration path is:

```text
libapp.so
  -> Flutter snapshot hash/features
  -> LiveVmContext bootstrap
  -> Library/Class/Function lookup
  -> Function* -> Code* -> DartCodePayload -> DartEntryTarget
  -> shared-code hook/listener validation
```

The fixture verifies that the shared physical Code entry used by
`instrumentedAdd` and `DartPlantFixture.addInt` fails closed without explicit
opt-in, accepts `DARTPLANT_HOOK_ALLOW_SHARED_CODE`, preserves requested listener
identity while marking logical identity ambiguous, and returns the expected
probe result `115`.

Run the device cold-start regression from the repository root:

```bash
python3 scripts/main.py test flutter-cold --flutter /path/to/flutter --rounds 30
```

The test rejects an APK that contains DartPlant runtime metadata.

## CI runtime scenarios

The Android activity accepts a `dartplant_test` string extra. Every supported
value produces one structured `DARTPLANT_CI` scenario result; `all` runs the
complete matrix sequentially in one Flutter isolate/session:

```bash
adb shell am start -W \
  -n dev.dartplant.dartplant_fixture/.MainActivity \
  --es dartplant_test all

adb shell am start -W \
  -n dev.dartplant.dartplant_fixture/.MainActivity \
  --es dartplant_test generic_gc
```

Supported scenarios are:

- `normal`: baseline live-VM discovery, ordinary typed hook/callback, null/bool
  semantics, and shared-code fail-close behavior.
- `arguments_descriptor`: generic closure invocation with positional/named
  `ArgumentsDescriptor` validation without requiring GC relocation.
- `closure`: implicit closure receiver and closure-call ABI validation.
- `generic_closure`: retained generic closure FunctionType/TypeArguments proof
  without requiring relocation.
- `generic_gc`: the same generic closure path while Dart API allocation
  pressure must relocate the authoritative rooted parameter and preserve its
  value.
- `exception`: generated exception observation/unwind and self-unhook lifetime
  behavior.
- `transition`: lazy source-verified Generated-to-Native transition proof.
- `artifact_revalidate`: quiesce, retire/invalidate, generation advance, and
  artifact revalidation proof.

`dartplant_probe=type_arguments` remains accepted as a compatibility alias for
`dartplant_test=generic_gc`, but new CI should use `dartplant_test`.
