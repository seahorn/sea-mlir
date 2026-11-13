// RUN: sea-opt %s --sea-lower-box-all=expand-max=0 | FileCheck %s
// RUN: sea-opt %s --sea-lower-box-all=expand-max=0 | sea-opt --sea-vc-lower | FileCheck %s --check-prefix=LOWERED
// sea-lower-box-all (docs/design/BoxAll.md): a claim in assert polarity
// becomes its body at one arbitrary index: a nondet t per dimension,
// assume(t ult N), k = lo + step * t, with expansion off (expand-max=0;
// expansion: lower-box-all-expand.mlir). The shapes: a constant box
// (straight-line), a box that may be empty (guarded), a constant empty
// box (true), a dead box (erased), and the accepted polarities.
// LOWERED-NOT: sea.box.all

// A constant box, lower bound 0, step 1: the index is the nondet.
// CHECK-LABEL: sea.program @const_box
// CHECK-SAME: expanded = 0 : i64, guarded = 0 : i64, lowered = 1 : i64, nondets = 1 : i64
// CHECK: %[[T:.*]] = sea.nondet "box.all.eq.0" [2] : i64
// CHECK: %[[LT:.*]] = sea.icmp ult, %[[T]], %{{.*}} : i64
// CHECK: sea.assume %[[LT]]
// CHECK: sea.ptr.add %{{.*}}, %[[T]]
// CHECK: %[[E:.*]] = sea.icmp eq
// CHECK: sea.assert %[[E]]
// CHECK-NOT: sea.box.all
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @const_box {
  %c8 = sea.const 8 : i64
  %z = sea.const 0 : i8
  %m = sea.mem.nondet "m" [0] : !m
  %a = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %all = sea.box.all (%i) = (0) to (%c8) {
    %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
    %x = sea.load %m[%p] : !m -> i8
    %e = sea.icmp eq, %x, %z : i8
    sea.yield %e
  } loc("eq")
  sea.assert %all
  sea.halt
}

// Lower bound 3, step 2, upper bound 10: N = (10 - 3 - 1) / 2 + 1 = 4,
// k = 2t + 3. Unnamed op: tag b<position>.
// CHECK-LABEL: sea.program @step
// CHECK: %[[N:.*]] = sea.const 4 : i64
// CHECK: %[[T:.*]] = sea.nondet "box.all.b0.0" [1] : i64
// CHECK: sea.icmp ult, %[[T]], %[[N]]
// CHECK: %[[M:.*]] = sea.mul %[[T]], %{{.*}} : i64
// CHECK: %[[K:.*]] = sea.add %[[M]], %{{.*}} : i64
// CHECK: sea.icmp ne, %[[K]],
sea.program @step {
  %x = sea.nondet "x" [0] : i64
  %c10 = sea.const 10 : i64
  %all = sea.box.all (%k) = (3) to (%c10) step (2) {
    %e = sea.icmp ne, %k, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// An SSA upper bound: the box may be empty, so it gets a block of its own,
// entered when n >u lo; the claim is the join's argument (1 on the empty
// side).
// CHECK-LABEL: sea.program @guarded
// CHECK-SAME: guarded = 1
// CHECK: %[[NE:.*]] = sea.icmp ugt, %[[NV:.*]], %{{.*}} : i64
// CHECK: %[[TRUE:.*]] = sea.const true
// CHECK: sea.cond_br %[[NE]], ^[[BOX:.*]], ^[[JOIN:.*]](%[[TRUE]] : i1)
// CHECK: ^[[BOX]]:
// CHECK: %[[T:.*]] = sea.nondet "box.all.b0.0"
// CHECK: sea.icmp ult, %[[T]], %[[NV]] : i64
// CHECK: sea.br ^[[JOIN]](%{{.*}} : i1)
// CHECK: ^[[JOIN]](%[[C:.*]]: i1):
// CHECK: sea.assert %[[C]]
sea.program @guarded {
  %n = sea.nondet "n" [0] : i64
  %x = sea.nondet "x" [1] : i64
  %all = sea.box.all (%i) = (0) to (%n) {
    %e = sea.icmp ne, %i, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// A constant empty box is true; a dead box is erased.
// CHECK-LABEL: sea.program @empty_and_dead
// CHECK-SAME: dead = 1 : i64, empty = 1 : i64
// CHECK-NOT: sea.nondet "box.all
// CHECK: sea.assert
sea.program @empty_and_dead {
  %c0 = sea.const 0 : i64
  %c5 = sea.const 5 : i64
  %all = sea.box.all (%i) = (5) to (%c5) {
    %f = sea.const 0 : i1
    sea.yield %f
  }
  %unused = sea.box.all (%i) = (0) to (%c5) {
    %f = sea.const 0 : i1
    sea.yield %f
  }
  sea.assert %all
  sea.halt
}

// Polarity: through and / or chains, the gated form or(not ok, all) (the
// negation is of `ok`, not of the claim), the product shape
// and(ok_l == ok_r, or(ok_l == 0, all)), several asserts, and two dimensions.
// CHECK-LABEL: sea.program @polarity
// CHECK-SAME: lowered = 2 : i64, nondets = 3 : i64
// CHECK-NOT: sea.box.all
sea.program @polarity {
  %okl = sea.nondet "okl" [0] : i1
  %okr = sea.nondet "okr" [1] : i1
  %x = sea.nondet "x" [2] : i64
  %c4 = sea.const 4 : i64
  %t = sea.const 1 : i1
  %z = sea.const 0 : i1
  %all = sea.box.all (%i) = (0) to (%c4) {
    %e = sea.icmp ne, %i, %x : i64
    sea.yield %e
  }
  %all2 = sea.box.all (%i, %j) = (0, 0) to (%c4, %c4) {
    %e = sea.icmp ne, %i, %j : i64
    sea.yield %e
  }
  %notok = sea.xor %okl, %t : i1
  %gated = sea.or %notok, %all : i1
  sea.assert %gated
  %same = sea.icmp eq, %okl, %okr : i1
  %off = sea.icmp eq, %okl, %z : i1
  %g2 = sea.or %off, %all2 : i1
  %prod = sea.and %same, %g2 : i1
  %both = sea.and %prod, %all : i1
  sea.assert %both
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
