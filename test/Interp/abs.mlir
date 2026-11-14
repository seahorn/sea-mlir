// RUN: sea-run %s --trace 2>/dev/null | FileCheck %s
// RUN: sea-run %s 2>&1 >/dev/null | FileCheck %s --check-prefix=WARN

// sea.abs (llvm.abs): ite(x <s 0, 0 - x, x); the minimal signed value is
// its own result. nsw (is_int_min_poison) does not change the value; the
// interpreter warns when the promise is broken.
sea.program @abs {
  %p = sea.const 100 : i8
  %n = sea.const -100 : i8
  %m = sea.const -128 : i8
  %n1 = sea.const -1 : i8
  %z = sea.const 0 : i8
  %one = sea.const 1 : i1
  // CHECK: value: sea.abs {{.*}} = 100
  %0 = sea.abs %p : i8
  // CHECK: value: sea.abs {{.*}} = 100
  %1 = sea.abs %n : i8
  // CHECK: value: sea.abs {{.*}} = 128
  %2 = sea.abs %m : i8
  // CHECK: value: sea.abs {{.*}} = 1
  %3 = sea.abs %n1 : i8
  // CHECK: value: sea.abs {{.*}} = 0
  %4 = sea.abs %z : i8
  // i1: -1 is the minimal signed value
  // CHECK: value: sea.abs {{.*}} = 1
  %5 = sea.abs %one : i1
  // only the INT_MIN input breaks the promise
  // WARN-NOT: interp.wrap-promise
  // WARN: abs.mlir":[[@LINE+4]]:{{.*}}[interp.wrap-promise]
  // CHECK: value: sea.abs {{.*}} = 100
  %6 = sea.abs %n nsw : i8
  // CHECK: value: sea.abs {{.*}} = 128
  %7 = sea.abs %m nsw : i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
