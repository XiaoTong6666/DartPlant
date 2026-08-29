// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#if defined(__aarch64__)
.text
.align 2
.global DartPlantDeviceInvokeDartCallbackWithOddSp
.type DartPlantDeviceInvokeDartCallbackWithOddSp, %function

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
#endif
