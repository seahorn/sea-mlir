// RUN: sea-opt %s --sea-expand-intrinsics | FileCheck %s
// RUN: sea-opt %s --sccp | FileCheck %s --check-prefix=FOLD

// Pins that sea.abs expands to a select of 0 - x and that nsw stays on the
// negation (values: utils/check-semantics.py --sea-opt).
// CHECK-LABEL: sea.program @p
// CHECK-NOT: sea.abs
// CHECK: sea.sub %{{.*}}, %{{.*}} : i32
// CHECK: sea.select
// CHECK: sea.sub %{{.*}}, %{{.*}} nsw : i32
// CHECK: sea.select
// Folds only without nsw (a promise, as on sea.sub).
// FOLD: sea.const 5 : i32
// FOLD: %[[C:.*]] = sea.const -5 : i32
// FOLD: sea.abs %[[C]] nsw : i32
sea.program @p {
  %c = sea.const -5 : i32
  %a = sea.nondet "nondet_u32" [0] : i32
  %0 = sea.abs %a : i32
  %1 = sea.abs %a nsw : i32
  %2 = sea.abs %c : i32
  %3 = sea.abs %c nsw : i32
  %s0 = sea.add %0, %1 : i32
  %s1 = sea.add %s0, %2 : i32
  %s2 = sea.add %s1, %3 : i32
  sea.print "r" %s2 : i32
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
