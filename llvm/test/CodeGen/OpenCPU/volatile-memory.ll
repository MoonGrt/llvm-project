; REQUIRES: opencpu-registered-target
; RUN: llc -march=opencpu -O0 -filetype=asm < %s | FileCheck %s
; CHECK-LABEL: main:
; CHECK: lw
; CHECK: sw
; CHECK: lw ra, 1(sp)
; CHECK: ret
; CHECK-NOT: call memcpy

@src = global [3 x i8] c"abc", align 1
@dst = global [3 x i8] zeroinitializer, align 1
declare void @llvm.memcpy.p0.p0.i16(ptr, ptr, i16, i1 immarg)
declare i16 @__opencpu_mmio_read16(i16)
define i16 @main() {
  call void @llvm.memcpy.p0.p0.i16(ptr @dst, ptr @src, i16 3, i1 true)
  %r = tail call i16 @__opencpu_mmio_read16(i16 -256)
  ret i16 %r
}
