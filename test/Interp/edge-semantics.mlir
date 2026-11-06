// RUN: sea-run %s --trace | FileCheck %s

// SMT-LIB total semantics at the edges (no UB). The z3 check in
// semantics-z3.test covers these systematically; this file documents them.
sea.program @edges {
  %z = sea.const 0 : i8
  %x = sea.const 100 : i8
  %m = sea.const -128 : i8
  %n1 = sea.const -1 : i8
  %w = sea.const 8 : i8
  %one = sea.const 1 : i8
  // bvudiv x 0 = ~0
  // CHECK: value: sea.udiv {{.*}} = 255
  %0 = sea.udiv %x, %z : i8
  // bvurem x 0 = x
  // CHECK: value: sea.urem {{.*}} = 100
  %1 = sea.urem %x, %z : i8
  // bvsdiv: nonnegative x / 0 = -1, negative x / 0 = 1
  // CHECK: value: sea.sdiv {{.*}} = 255
  %2 = sea.sdiv %x, %z : i8
  // CHECK: value: sea.sdiv {{.*}} = 1
  %3 = sea.sdiv %m, %z : i8
  // INT_MIN / -1 wraps to INT_MIN
  // CHECK: value: sea.sdiv {{.*}} = 128
  %4 = sea.sdiv %m, %n1 : i8
  // bvsrem x 0 = x
  // CHECK: value: sea.srem {{.*}} = 128
  %5 = sea.srem %m, %z : i8
  // shifts by >= width
  // CHECK: value: sea.shl {{.*}} = 0
  %6 = sea.shl %one, %w : i8
  // CHECK: value: sea.lshr {{.*}} = 0
  %7 = sea.lshr %m, %w : i8
  // CHECK: value: sea.ashr {{.*}} = 255
  %8 = sea.ashr %m, %w : i8
  // CHECK: value: sea.ashr {{.*}} = 0
  %9 = sea.ashr %x, %n1 : i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
