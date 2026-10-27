; REQUIRES: opencpu-registered-target
; RUN: not llc -march=opencpu -O0 < %s 2>&1 | FileCheck %s
; CHECK: LLVM ERROR: OpenCPU: unsupported IR instruction: sdiv
; CHECK-NOT: Stack dump
; CHECK-NOT: PLEASE submit a bug report

define i16 @main() {
  %r = sdiv i16 42, 2
  ret i16 %r
}
