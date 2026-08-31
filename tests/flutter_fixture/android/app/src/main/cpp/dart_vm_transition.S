#if defined(__aarch64__)
.text
.align 2
.global dartplant_fixture_call_vm_safepoint_stub
.type dartplant_fixture_call_vm_safepoint_stub, %function
dartplant_fixture_call_vm_safepoint_stub:
  // VM safepoint stubs use Dart's SPREG (x15) while CSP remains parked below
  // it. A normal C++ caller may freely clobber x15, so provide a private Dart
  // stack window instead of exposing either the intercepted Dart frame or the
  // helper's saved registers to the stub.
  sub sp, sp, #1024
  stp x15, x26, [sp]
  str x30, [sp, #16]
  add x15, sp, #1024
  mov x26, x0
  blr x1
  ldr x30, [sp, #16]
  ldp x15, x26, [sp]
  add sp, sp, #1024
  ret
.size dartplant_fixture_call_vm_safepoint_stub, .-dartplant_fixture_call_vm_safepoint_stub
#endif
