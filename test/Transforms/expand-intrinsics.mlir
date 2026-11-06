// RUN: sea-opt %s --sea-expand-intrinsics | FileCheck %s

// Pins only that the intrinsics are gone and core ops remain; the
// expansion's values are checked by utils/check-semantics.py --sea-opt.
// CHECK-LABEL: sea.program @p
// CHECK-NOT: sea.umax
// CHECK-NOT: sea.bswap
// CHECK-NOT: sea.ctpop
// CHECK-NOT: sea.fshl
// CHECK-NOT: sea.smulo
// CHECK: sea.halt
sea.program @p {
  %a = sea.nondet "nondet_u32" [0] : i32
  %b = sea.nondet "nondet_u32" [1] : i32
  %0 = sea.umax %a, %b : i32
  %1 = sea.bswap %0 : i32
  %ctp = sea.ctpop %0 : i32
  %2 = sea.fshl %1, %a, %b : i32
  %3 = sea.smulo %2, %b : i32
  sea.assert %3
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
