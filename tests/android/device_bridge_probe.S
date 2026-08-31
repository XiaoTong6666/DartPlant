// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#if defined(__aarch64__)
.text
.align 2
.global DartPlantDeviceInvokeDartCallbackWithOddSp
.type DartPlantDeviceInvokeDartCallbackWithOddSp, %function
.global DartPlantDeviceInvokeGeneratedGate
.type DartPlantDeviceInvokeGeneratedGate, %function

// Deterministic device-only probe for the real-Dart callback entry's
// x15%16==8 alignment branch.
//   x0 = DartPlantHook*
//   x1 = synthetic Dart SPREG (must end in ...8)
//   x2 = raw x0 argument value observed by the callback
// Tail-branch so x30 remains the C caller's true return address. The callback
// skips the original, restores the parked C SP, and returns directly to C++.
DartPlantDeviceInvokeDartCallbackWithOddSp:
  mov x17, x0
  mov x15, x1
  mov x0, x2
  b dartplant_arm64_callback_entry

.size DartPlantDeviceInvokeDartCallbackWithOddSp, .-DartPlantDeviceInvokeDartCallbackWithOddSp

// Execute an arbitrary entry through the generated-Dart publication-gate ABI
// while the surrounding test is still ordinary native C++.
//   x0 = target/gate entry
//   x1 = synthetic writable Dart SPREG
//   x2 = first native integer argument
//   x3 = second native integer argument
// Tail-branch so x30 remains the native caller's return address. The gate uses
// x15 as Dart SPREG only for its temporary save area, restores the parked native
// CSP before branching onward, and leaves x0/x1 as the callee's real arguments.
DartPlantDeviceInvokeGeneratedGate:
  mov x16, x0
  mov x15, x1
  mov x0, x2
  mov x1, x3
  br x16

.size DartPlantDeviceInvokeGeneratedGate, .-DartPlantDeviceInvokeGeneratedGate
#endif
