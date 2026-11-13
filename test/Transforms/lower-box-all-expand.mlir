// RUN: sea-opt %s --sea-lower-box-all=expand-max=4 | FileCheck %s
// RUN: sea-opt %s --sea-lower-box-all=expand-max=4 | sea-opt --sea-vc-lower | FileCheck %s --check-prefix=LOWERED
// RUN: not sea-opt %s --sea-lower-box-all=expand-max=65537 2>&1 | FileCheck %s --check-prefix=LIMIT
// RUN: not sea-vc %s --program=one --lower --box-all-expand=65537 2>&1 | FileCheck %s --check-prefix=LIMIT
// LIMIT: is above the limit (65536)
// sea-lower-box-all{expand-max=K} (docs/design/BoxAll.md): a box whose
// upper bounds are constants and whose trip count is at most K becomes the
// body at every index, row-major, and-reduced; any other box gets one
// nondet index. The polarity rule does not depend on K
// (lower-box-all-errors.mlir runs with the default K).
// LOWERED-NOT: sea.box.all

// Indices 0, 1, 2: three copies of the body, two ands, no nondet.
// CHECK-LABEL: sea.program @one
// CHECK-SAME: expanded = 1 : i64
// CHECK-NOT: sea.nondet "box.all
// CHECK: %[[K0:.*]] = sea.const 0 : i64
// CHECK: %[[E0:.*]] = sea.icmp ne, %[[K0]], %[[X:.*]] : i64
// CHECK: %[[K1:.*]] = sea.const 1 : i64
// CHECK: %[[E1:.*]] = sea.icmp ne, %[[K1]], %[[X]] : i64
// CHECK: %[[A1:.*]] = sea.and %[[E0]], %[[E1]] : i1
// CHECK: %[[K2:.*]] = sea.const 2 : i64
// CHECK: %[[E2:.*]] = sea.icmp ne, %[[K2]], %[[X]] : i64
// CHECK: %[[A2:.*]] = sea.and %[[A1]], %[[E2]] : i1
// CHECK: sea.assert %[[A2]]
sea.program @one {
  %x = sea.nondet "x" [0] : i64
  %c3 = sea.const 3 : i64
  %all = sea.box.all (%i) = (0) to (%c3) {
    %e = sea.icmp ne, %i, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// Two dimensions with a lower bound and a step, an asymmetric body
// 10i + j: (1, 0) (1, 1) (3, 0) (3, 1), row-major (dimension 0 outer);
// 4 indices = K: expanded.
// CHECK-LABEL: sea.program @two
// CHECK-SAME: expanded = 1 : i64
// CHECK-NOT: sea.nondet "box.all
// CHECK: %[[I0:.*]] = sea.const 1 : i64
// CHECK-NEXT: %[[J0:.*]] = sea.const 0 : i64
// CHECK-NEXT: %[[ROW0:.*]] = sea.mul %[[I0]], %{{.*}} : i64
// CHECK-NEXT: sea.add %[[ROW0]], %[[J0]] : i64
// CHECK: %[[I1:.*]] = sea.const 1 : i64
// CHECK-NEXT: %[[J1:.*]] = sea.const 1 : i64
// CHECK-NEXT: %[[ROW1:.*]] = sea.mul %[[I1]], %{{.*}} : i64
// CHECK-NEXT: sea.add %[[ROW1]], %[[J1]] : i64
// CHECK: sea.and
// CHECK: %[[I2:.*]] = sea.const 3 : i64
// CHECK-NEXT: %[[J2:.*]] = sea.const 0 : i64
// CHECK-NEXT: %[[ROW2:.*]] = sea.mul %[[I2]], %{{.*}} : i64
// CHECK-NEXT: sea.add %[[ROW2]], %[[J2]] : i64
// CHECK: sea.and
// CHECK: %[[I3:.*]] = sea.const 3 : i64
// CHECK-NEXT: %[[J3:.*]] = sea.const 1 : i64
// CHECK-NEXT: %[[ROW3:.*]] = sea.mul %[[I3]], %{{.*}} : i64
// CHECK-NEXT: sea.add %[[ROW3]], %[[J3]] : i64
// CHECK: sea.and
// CHECK-NOT: sea.and
sea.program @two {
  %x = sea.nondet "x" [0] : i64
  %c2 = sea.const 2 : i64
  %c4 = sea.const 4 : i64
  %c10 = sea.const 10 : i64
  %all = sea.box.all (%i, %j) = (1, 0) to (%c4, %c2) step (2, 1) {
    %r = sea.mul %i, %c10 : i64
    %s = sea.add %r, %j : i64
    %e = sea.icmp ne, %s, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// 3 x 2 = 6 indices > K, though each dimension is <= K: one index.
// CHECK-LABEL: sea.program @product
// CHECK-SAME: expanded = 0 : i64
// CHECK-SAME: lowered = 1 : i64
sea.program @product {
  %x = sea.nondet "x" [0] : i64
  %c2 = sea.const 2 : i64
  %c3 = sea.const 3 : i64
  %all = sea.box.all (%i, %j) = (0, 0) to (%c3, %c2) {
    %s = sea.add %i, %j : i64
    %e = sea.icmp ne, %s, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// One constant and one symbolic upper bound: not expanded (guarded).
// CHECK-LABEL: sea.program @mixed
// CHECK-SAME: expanded = 0 : i64
// CHECK-SAME: guarded = 1 : i64
sea.program @mixed {
  %n = sea.nondet "n" [0] : i64
  %x = sea.nondet "x" [1] : i64
  %c2 = sea.const 2 : i64
  %all = sea.box.all (%i, %j) = (0, 0) to (%c2, %n) {
    %s = sea.add %i, %j : i64
    %e = sea.icmp ne, %s, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// A bound computed with nsw is not a constant to the pass (ops with flags
// do not fold; it runs before sccp): one index, though it is 2.
// CHECK-LABEL: sea.program @flagged
// CHECK-SAME: expanded = 0 : i64
// CHECK-SAME: lowered = 1 : i64
sea.program @flagged {
  %x = sea.nondet "x" [0] : i64
  %c1 = sea.const 1 : i64
  %n = sea.add %c1, %c1 nsw : i64
  %all = sea.box.all (%i) = (0) to (%n) {
    %e = sea.icmp ne, %i, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// 5 indices > K: one nondet index.
// CHECK-LABEL: sea.program @over
// CHECK-SAME: expanded = 0 : i64
// CHECK-SAME: lowered = 1 : i64
// CHECK: sea.nondet "box.all.b0.0"
sea.program @over {
  %x = sea.nondet "x" [0] : i64
  %c5 = sea.const 5 : i64
  %all = sea.box.all (%i) = (0) to (%c5) {
    %e = sea.icmp ne, %i, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// A symbolic bound is never expanded, even when it is assumed small.
// CHECK-LABEL: sea.program @symbolic
// CHECK-SAME: expanded = 0 : i64
// CHECK-SAME: guarded = 1 : i64
// CHECK: sea.nondet "box.all.b0.0"
sea.program @symbolic {
  %n = sea.nondet "n" [0] : i64
  %x = sea.nondet "x" [1] : i64
  %c2 = sea.const 2 : i64
  %small = sea.icmp ult, %n, %c2 : i64
  sea.assume %small
  %all = sea.box.all (%i) = (0) to (%n) {
    %e = sea.icmp ne, %i, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
