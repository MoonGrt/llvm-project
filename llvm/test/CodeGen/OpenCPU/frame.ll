; REQUIRES: opencpu-registered-target
; RUN: llc -march=opencpu -O0 -filetype=asm < %s | FileCheck %s
; CHECK-LABEL: main:
; CHECK: sw fp, 0(sp)
; CHECK: sw ra, 1(sp)
; CHECK: addi fp,
; CHECK: sw
; CHECK: lw
; CHECK: lw ra, 1(sp)
; CHECK: lw fp, 0(sp)
; CHECK: ret

define i16 @main() {
  %a = alloca [40 x i16], align 2
  %p = getelementptr [40 x i16], ptr %a, i32 0, i32 39
  store volatile i16 123, ptr %p, align 2
  %r = load volatile i16, ptr %p, align 2
  ret i16 %r
}
