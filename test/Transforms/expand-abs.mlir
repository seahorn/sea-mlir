// RUN: sea-opt %s --sea-expand-intrinsics | FileCheck %s
// RUN: sea-opt %s --sccp | FileCheck %s --check-prefix=FOLD

// Pins that sea.abs expands to a select of 0 - x and that nsw stays on the
// negation (values: utils/check-semantics.py --sea-opt).
// CHECK-LABEL: sea.program @p
// CHECK-NOT: sea.abs
// CHECK: sea.sub %{{[^ ]+}}, %{{[^ ]+}} : i32
// CHECK: sea.select
// CHECK: sea.sub %{{.*}}, %{{.*}} nsw : i32
// CHECK: sea.select
// Folds without nsw, and with nsw when the promise holds of the constant
// (-5); not of INT_MIN, whose promise breaks (a promise, as on sea.sub).
// FOLD-DAG: sea.const 5 : i32
// FOLD-DAG: %[[M:.*]] = sea.const -2147483648 : i32
// The nondet's two abs ops and INT_MIN's stay; both abs of -5 (with and
// without nsw) are gone:
// FOLD: sea.abs %{{.*}} : i32
// FOLD-NEXT: sea.abs %{{.*}} nsw : i32
// FOLD-NEXT: sea.abs %[[M]] nsw : i32
// FOLD-NOT: sea.abs
sea.program @p {
  %c = sea.const -5 : i32
  %a = sea.nondet "nondet_u32" [0] : i32
  %0 = sea.abs %a : i32
  %1 = sea.abs %a nsw : i32
  %2 = sea.abs %c : i32
  %3 = sea.abs %c nsw : i32
  %mn = sea.const -2147483648 : i32
  %4 = sea.abs %mn nsw : i32
  %s0 = sea.add %0, %1 : i32
  %s1 = sea.add %s0, %2 : i32
  %s2 = sea.add %s1, %3 : i32
  %s3 = sea.add %s2, %4 : i32
  sea.print "r" %s3 : i32
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
