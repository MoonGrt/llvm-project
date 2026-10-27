; REQUIRES: opencpu-registered-target
; RUN: llc -march=opencpu -O0 -filetype=asm < %s | FileCheck %s
; The backend emits the OpenCPU flat word assembler dialect, not GAS.
; CHECK-NOT: .text
; CHECK-NOT: .globl
; CHECK-LABEL: add_one:
; CHECK: add
; CHECK: ret
; CHECK-LABEL: main:
; CHECK: call add_one
; CHECK: ret

define i16 @add_one(i16 %x) {
  %r = add i16 %x, 1
  ret i16 %r
}
define i16 @main() {
  %r = call i16 @add_one(i16 41)
  ret i16 %r
}
