; RUN: llc -march=opencpu -O0 -verify-machineinstrs -filetype=asm < %s | FileCheck %s
target triple = "opencpu-unknown-none"
declare i16 @__opencpu_mmio_read16(i16)
; CHECK-LABEL: main:
; CHECK: beq {{(a[1-4], zero|zero, a[1-4])}},
define i16 @main() {
  %value = call i16 @__opencpu_mmio_read16(i16 65280)
  %zero = icmp eq i16 %value, 0
  br i1 %zero, label %yes, label %no
yes:
  ret i16 1
no:
  ret i16 2
}
