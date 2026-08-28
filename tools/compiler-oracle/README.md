# DartPlant compiler ABI oracle

`dump_abi_oracle.dart` exports normalized Function-slot ABI representations
and register-CC facts from the Dart compiler's `vm.unboxing-info.metadata` and
the matching transformed Kernel member shape.

The script must be run with the Dart executable and package configuration from
the same Dart SDK checkout/compiler pipeline that produced the transformed DILL
used for the target AOT build. It is not a runtime dependency and it never
infers machine representation from `FunctionType` or source-language types.

The output uses DartPlant's normalized representation vocabulary:

- `tagged`
- `unboxed-int64`
- `unboxed-double`
- `pair-of-tagged` (record return)
- `unknown`

If the matching transformed DILL/compiler metadata is unavailable, the caller
must treat the ABI as unknown instead of substituting a guessed representation.

This producer intentionally emits only **compiler truth**. A runtime-ready
evidence record must additionally bind those facts to the exact target artifact:

- snapshot hash;
- `libapp.so` build-id;
- physical Dart Code fingerprint.

Those identity fields come from the matching AOT artifact/indexing step (for
example DartPlant's offline indexer), not from `FunctionType` and not from this
Kernel metadata reader. `dartplant_runtime_register_compiler_abi_evidence()`
requires all three and revalidates them against the live runtime before a
`DartCallLayout` can become verified.

For ordinary Kernel members, the producer mirrors the same rules used by
`Function::MaxNumberOfParametersInRegisters()`: generic functions and
`mustUseStackCallingConvention` use zero register parameters; getters/setters
use their fixed parameter count; the override-shortening case uses the
compiler metadata's direct-parameter count plus the receiver. Instance
receivers are emitted as an implicit tagged fixed parameter.

The absence of `vm.unboxing-info.metadata` for a member is meaningful: the Dart
compiler omits `UnboxingInfoMetadata.isTrivial`, so on the exact transformed
DILL it proves boxed/tagged fixed parameters and return with default flags.
Explicit `unknown` entries in present metadata remain `unknown`.

Closures and synthetic dispatchers are intentionally outside this producer's
current enumeration. `has_optional_parameters` is emitted so the runtime-side
consumer can keep typed frames fail-closed until optional argument transport is
implemented.
