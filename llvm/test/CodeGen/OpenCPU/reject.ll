; REQUIRES: opencpu-registered-target
; RUN: not llc -march=opencpu -O0 < %s 2>&1 | FileCheck %s
; CHECK: LLVM ERROR: OpenCPU: this module requires -opencpu-runtime=
; CHECK-NOT: Stack dump
; CHECK-NOT: PLEASE submit a bug report

define i16 @divide(i16 %value) {
  %r = sdiv i16 %value, 2
  ret i16 %r
}
define i16 @main() {
  %r = call i16 @divide(i16 42)
  ret i16 %r
}
