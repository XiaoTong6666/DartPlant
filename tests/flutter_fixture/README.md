# dartplant_fixture

Fixed Flutter 3.22.3 / Dart 3.4.4 ARM64 release fixture for DartPlant.

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
