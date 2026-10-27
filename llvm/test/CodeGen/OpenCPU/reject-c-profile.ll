; REQUIRES: opencpu-registered-target
; RUN: not llc -march=opencpu -O0 < %s 2>&1 | FileCheck %s
; CHECK: OpenCPU: alloca requires a fixed integer/pointer object
; CHECK-NOT: Stack dump

define i16 @main() {
  %n = call i16 @count()
  %a = alloca i16, i16 %n, align 2
  store volatile i16 4, ptr %a, align 2
  ret i16 0
}
define i16 @count() { ret i16 7 }
