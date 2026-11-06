// RUN: sea-opt %s --sea-lower-ptr-index 2>&1 | FileCheck %s
// RUN: sea-opt %s --sea-lower-ptr-index | sea-run - --program=lowered --trace | FileCheck %s --check-prefix=SAME
// RUN: sea-run %s --program=lowered --trace | FileCheck %s --check-prefix=SAME

// sea-lower-ptr-index (docs/design/PtrIndex.md): sext, mul, add, then
// addr.add / ptr.add; the value unchanged (the interpreter's trace before
// and after); the flags dropped (reported), not mapped onto nsw / nuw.
// CHECK: Warning: 1 sea.ptr.index no-wrap flag set(s) dropped by sea-lower-ptr-index (precision only) [lower-ptr-index.dropped-flags]
// CHECK-LABEL: sea.program @lowered
// CHECK-NOT: sea.ptr.index
// CHECK: %[[S0:.*]] = sea.const 24 : i8
// CHECK: %[[T0:.*]] = sea.mul %{{.*}}, %[[S0]] : i8
// CHECK: %[[J:.*]] = sea.sext %{{.*}} : i4 to i8
// CHECK: %[[S1:.*]] = sea.const -4 : i8
// CHECK: %[[T1:.*]] = sea.mul %[[J]], %[[S1]] : i8
// CHECK: %[[O:.*]] = sea.add %[[T0]], %[[T1]] : i8
// CHECK: %[[C:.*]] = sea.const 16 : i8
// CHECK: %[[OC:.*]] = sea.add %[[O]], %[[C]] : i8
// CHECK: sea.addr.add %{{.*}}, %[[OC]] : !sea.addr<8>, i8
// CHECK-NOT: nsw
// CHECK: sea.ptr.add %{{.*}}, %{{.*}} : !sea.ptr<64>, i64
// SAME: value: sea.addr.to_int {{.*}} = 196
// SAME: value: sea.addr.to_int {{.*}} = 56
sea.program @lowered {
  %b = sea.const 100 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 3 : i8
  %j = sea.const -2 : i4
  %q = sea.ptr.index %a [%i, %j] strides [24, -4] offset 16 inbounds : !sea.addr<8>, i8, i4
  %qi = sea.addr.to_int %q : !sea.addr<8> to i8
  %r = sea.ptr.index %a [%i] strides [-8] offset -20 : !sea.addr<8>, i8
  %ri = sea.addr.to_int %r : !sea.addr<8> to i8
  %n = sea.const 64 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %k = sea.const 2 : i64
  %f = sea.ptr.index %p [%k] strides [8] offset 4 : !sea.ptr<64>, i64
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
