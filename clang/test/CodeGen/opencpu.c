// RUN: %clang_cc1 -triple opencpu-unknown-none -ffreestanding -emit-llvm -o - %s | FileCheck %s
// CHECK: target datalayout = "e-m:e-p:16:16-i32:16-i64:16-f32:16-f64:16-n16-S16"
_Static_assert(sizeof(int) == 2, "int16");
_Static_assert(sizeof(long) == 4 && _Alignof(long) == 2, "long32");
_Static_assert(sizeof(long long) == 8 && _Alignof(long long) == 2, "longlong64");
_Static_assert(sizeof(void *) == 2, "pointer16");
_Static_assert(sizeof(__CHAR32_TYPE__) == 4, "char32");
// CHECK-LABEL: define{{.*}} i16 @identity(i16
int identity(int x) { return x; }
// CHECK-LABEL: define{{.*}} i32 @wide(i32
long wide(long x) { return x; }
// CHECK-LABEL: define{{.*}} i64 @wider(i64
long long wider(long long x) { return x; }
// CHECK-LABEL: define{{.*}} @mmio(
// CHECK: call i16 @__opencpu_mmio_read16(i16
// CHECK: call void @__opencpu_mmio_write16(i16
unsigned mmio(unsigned address) {
  unsigned value = __builtin_opencpu_mmio_read16(address);
  __builtin_opencpu_mmio_write16(address, value);
  return value;
}
