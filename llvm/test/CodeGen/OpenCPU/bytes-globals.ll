; REQUIRES: opencpu-registered-target
; RUN: llc -march=opencpu -O0 -filetype=asm < %s | FileCheck %s
; CHECK: # opencpu-data-size: 6
; CHECK-LABEL: main:
; CHECK: lw
; CHECK: sw
; CHECK: ret
; CHECK-LABEL: __opencpu_init_data:
; CHECK: sw

@bytes = global [3 x i8] c"abc", align 1
define i16 @main() {
  %p = getelementptr [3 x i8], ptr @bytes, i16 0, i16 1
  %b = load volatile i8, ptr %p, align 1
  store volatile i8 7, ptr %p, align 1
  %r = zext i8 %b to i16
  ret i16 %r
}
