// RUN: %clang_cc1 -triple opencpu-unknown-none -ffreestanding -E -dM %s | FileCheck %s
// CHECK-DAG: #define __OPENCPU__ 1
// CHECK-DAG: #define __OPENCPU_ABI__ 2
// CHECK-DAG: #define __CHAR_BIT__ 8
// CHECK-DAG: #define __SIZEOF_SHORT__ 2
// CHECK-DAG: #define __SIZEOF_INT__ 2
// CHECK-DAG: #define __SIZEOF_LONG__ 4
// CHECK-DAG: #define __SIZEOF_LONG_LONG__ 8
// CHECK-DAG: #define __SIZEOF_POINTER__ 2
// CHECK-DAG: #define __STDC_HOSTED__ 0
// CHECK-DAG: #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
