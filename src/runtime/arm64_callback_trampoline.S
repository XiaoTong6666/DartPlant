// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#if defined(__aarch64__)

.text
.align 2
.global dartplant_arm64_callback_entry
.type dartplant_arm64_callback_entry, %function
.global dartplant_arm64_continuation
.type dartplant_arm64_continuation, %function
.global dartplant_arm64_invoke_original
.type dartplant_arm64_invoke_original, %function

// Stack layout:
//   [0, 248)   DartPlantArm64Context.x[31]
//   [248, 256) original SP
//   [256, 264) PC placeholder
//   [264, 272) PSTATE placeholder
//   [272, 784) SIMD registers q0..q31
//   frame size: 816 bytes

dartplant_arm64_callback_entry:
  // The generated veneer puts the Hook pointer in x17 and uses x16 as IP0.
  // Both are AArch64 scratch registers and are intentionally not preserved.
  sub sp, sp, #816
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp x8, x9, [sp, #64]
  stp x10, x11, [sp, #80]
  stp x12, x13, [sp, #96]
  stp x14, x15, [sp, #112]
  stp x16, x17, [sp, #128]
  stp x18, x19, [sp, #144]
  stp x20, x21, [sp, #160]
  stp x22, x23, [sp, #176]
  stp x24, x25, [sp, #192]
  stp x26, x27, [sp, #208]
  stp x28, x29, [sp, #224]
  str x30, [sp, #240]
  add x9, sp, #816
  str x9, [sp, #248]
  adr x9, .Ldartplant_continuation
  str x9, [sp, #256]
  str xzr, [sp, #264]
  stp q0, q1, [sp, #272]
  stp q2, q3, [sp, #304]
  stp q4, q5, [sp, #336]
  stp q6, q7, [sp, #368]
  stp q8, q9, [sp, #400]
  stp q10, q11, [sp, #432]
  stp q12, q13, [sp, #464]
  stp q14, q15, [sp, #496]
  stp q16, q17, [sp, #528]
  stp q18, q19, [sp, #560]
  stp q20, q21, [sp, #592]
  stp q22, q23, [sp, #624]
  stp q24, q25, [sp, #656]
  stp q26, q27, [sp, #688]
  stp q28, q29, [sp, #720]
  stp q30, q31, [sp, #752]
  mov x0, sp
  mov x1, x17
  bl dartplant_arm64_dispatch_enter
  cbz x1, .Ldartplant_skip

  // x1 is the backup entry and x0 is the TLS-backed context. Keep the context
  // in x14; x14/x15 are caller-saved and may be used by this bridge.
  mov x14, x0
  str x1, [x14, #264]
  ldp x0, x1, [x14, #0]
  ldp x2, x3, [x14, #16]
  ldp x4, x5, [x14, #32]
  ldp x6, x7, [x14, #48]
  ldp x8, x9, [x14, #64]
  ldp x10, x11, [x14, #80]
  ldp x12, x13, [x14, #96]
  ldp x18, x19, [x14, #144]
  ldp x20, x21, [x14, #160]
  ldp x22, x23, [x14, #176]
  ldp x24, x25, [x14, #192]
  ldp x26, x27, [x14, #208]
  ldp x28, x29, [x14, #224]
  ldp q0, q1, [x14, #272]
  ldp q2, q3, [x14, #304]
  ldp q4, q5, [x14, #336]
  ldp q6, q7, [x14, #368]
  ldp q8, q9, [x14, #400]
  ldp q10, q11, [x14, #432]
  ldp q12, q13, [x14, #464]
  ldp q14, q15, [x14, #496]
  ldp q16, q17, [x14, #528]
  ldp q18, q19, [x14, #560]
  ldp q20, q21, [x14, #592]
  ldp q22, q23, [x14, #624]
  ldp q24, q25, [x14, #656]
  ldp q26, q27, [x14, #688]
  ldp q28, q29, [x14, #720]
  ldp q30, q31, [x14, #752]
  ldr x30, [x14, #256]
  ldr x17, [x14, #264]
  ldp x14, x15, [x14, #112]
  br x17

.Ldartplant_skip:
  mov x14, x0
  b .Ldartplant_restore_and_return

dartplant_arm64_continuation:
.Ldartplant_continuation:
  // x0 is the original return word. The C dispatcher updates context.x[0]
  // before invoking the leave callback and returns the context plus result.
  bl dartplant_arm64_dispatch_leave_from_tls
  mov x14, x0
.Ldartplant_restore_and_return:
  ldp x0, x1, [x14, #0]
  ldp x2, x3, [x14, #16]
  ldp x4, x5, [x14, #32]
  ldp x6, x7, [x14, #48]
  ldp x8, x9, [x14, #64]
  ldp x10, x11, [x14, #80]
  ldp x12, x13, [x14, #96]
  ldp x18, x19, [x14, #144]
  ldp x20, x21, [x14, #160]
  ldp x22, x23, [x14, #176]
  ldp x24, x25, [x14, #192]
  ldp x26, x27, [x14, #208]
  ldp x28, x29, [x14, #224]
  ldr x30, [x14, #240]
  ldp q0, q1, [x14, #272]
  ldp q2, q3, [x14, #304]
  ldp q4, q5, [x14, #336]
  ldp q6, q7, [x14, #368]
  ldp q8, q9, [x14, #400]
  ldp q10, q11, [x14, #432]
  ldp q12, q13, [x14, #464]
  ldp q14, q15, [x14, #496]
  ldp q16, q17, [x14, #528]
  ldp q18, q19, [x14, #560]
  ldp q20, q21, [x14, #592]
  ldp q22, q23, [x14, #624]
  ldp q24, q25, [x14, #656]
  ldp q26, q27, [x14, #688]
  ldp q28, q29, [x14, #720]
  ldp q30, q31, [x14, #752]
  ldr x30, [x14, #240]
  ldp x14, x15, [x14, #112]
  add sp, sp, #816
  ret

.size dartplant_arm64_callback_entry, .-dartplant_arm64_callback_entry
.size dartplant_arm64_continuation, .-dartplant_arm64_continuation

// Called from a native enter callback. It invokes the original Dart entry
// synchronously, but keeps the native C ABI callee-saved registers intact.
// x0 = DartPlantArm64Context*, x1 = original entry.
dartplant_arm64_invoke_original:
  sub sp, sp, #256
  stp x19, x20, [sp, #0]
  stp x21, x22, [sp, #16]
  stp x23, x24, [sp, #32]
  stp x25, x26, [sp, #48]
  stp x27, x28, [sp, #64]
  stp x29, x30, [sp, #80]
  stp q8, q9, [sp, #96]
  stp q10, q11, [sp, #128]
  stp q12, q13, [sp, #160]
  stp q14, q15, [sp, #192]
  str x0, [sp, #224]
  str x1, [sp, #232]

  mov x19, x0
  ldp x0, x1, [x19, #0]
  ldp x2, x3, [x19, #16]
  ldp x4, x5, [x19, #32]
  ldp x6, x7, [x19, #48]
  ldp x8, x9, [x19, #64]
  ldp x10, x11, [x19, #80]
  ldp x12, x13, [x19, #96]
  ldp x14, x15, [x19, #112]
  ldp x16, x17, [x19, #128]
  ldr x18, [x19, #144]
  ldp x20, x21, [x19, #160]
  ldp x22, x23, [x19, #176]
  ldp x24, x25, [x19, #192]
  ldp x26, x27, [x19, #208]
  ldp x28, x29, [x19, #224]
  ldp q0, q1, [x19, #272]
  ldp q2, q3, [x19, #304]
  ldp q4, q5, [x19, #336]
  ldp q6, q7, [x19, #368]
  ldp q8, q9, [x19, #400]
  ldp q10, q11, [x19, #432]
  ldp q12, q13, [x19, #464]
  ldp q14, q15, [x19, #496]
  ldp q16, q17, [x19, #528]
  ldp q18, q19, [x19, #560]
  ldp q20, q21, [x19, #592]
  ldp q22, q23, [x19, #624]
  ldp q24, q25, [x19, #656]
  ldp q26, q27, [x19, #688]
  ldp q28, q29, [x19, #720]
  ldp q30, q31, [x19, #752]
  ldr x19, [x19, #152]
  adr x30, .Ldartplant_original_return
  ldr x16, [sp, #232]
  br x16

.Ldartplant_original_return:
  ldr x19, [sp, #224]
  stp x0, x1, [x19, #0]
  stp q0, q1, [x19, #272]
  ldp x19, x20, [sp, #0]
  ldp x21, x22, [sp, #16]
  ldp x23, x24, [sp, #32]
  ldp x25, x26, [sp, #48]
  ldp x27, x28, [sp, #64]
  ldp x29, x30, [sp, #80]
  ldp q8, q9, [sp, #96]
  ldp q10, q11, [sp, #128]
  ldp q12, q13, [sp, #160]
  ldp q14, q15, [sp, #192]
  add sp, sp, #256
  ret

#endif
