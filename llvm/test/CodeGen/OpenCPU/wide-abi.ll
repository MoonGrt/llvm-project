; REQUIRES: opencpu-registered-target
; RUN: llc -march=opencpu -O0 -filetype=asm < %s | FileCheck %s
; CHECK-LABEL: many:
; CHECK: lw
; CHECK: add
; CHECK: ret
; CHECK-LABEL: main:
; CHECK: call many
; CHECK: ret

define i64 @many(i64 %x, i64 %y) {
  %r = add i64 %x, %y
  ret i64 %r
}
define i16 @main() {
  %n = call i64 @many(i64 65535, i64 65537)
  %r = trunc i64 %n to i16
  ret i16 %r
}
