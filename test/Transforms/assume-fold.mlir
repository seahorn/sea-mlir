// RUN: sea-opt %s --sea-assume-fold | FileCheck %s
// sea-assume-fold (docs/design/AssumeFold.md): after `sea.assume %c` or
// `sea.assert %c`, the uses of %c (and of what %c fixes) in ops the check
// dominates read a constant. Branches and asserts stay: canonicalize and
// sea-merge-asserts fold them. One program per property; the counters are
// on the program.

// The reduced shape: assume %c, then cond_br %c and a select on %c.
// CHECK-LABEL: sea.program @direct
// CHECK-SAME: sea.assume_fold = {"asserts-failed" = 0 : i64, "asserts-held" = 0 : i64, blocks = 1 : i64, branches = 1 : i64, "by-assert" = 0 : i64, "by-assume" = 2 : i64, selects = 1 : i64, uses = 2 : i64, values = 1 : i64}
// CHECK: %[[T:.*]] = sea.const true
// CHECK: sea.assume %[[C:[^ ]*]]
// CHECK: sea.select %[[T]],
// CHECK: sea.cond_br %[[T]], ^bb1, ^bb2
sea.program @direct {
  %a = sea.const 1 : i8
  %b = sea.const 2 : i8
  %c = sea.nondet "c" [0] : i1
  %x = sea.nondet "x" [1] : i8
  sea.assume %c
  %s = sea.select %c, %a, %b : i8
  sea.cond_br %c, ^bb1, ^bb2
^bb1:
  sea.br ^bb3
^bb2:
  sea.br ^bb3
^bb3:
  %ok = sea.icmp ne, %x, %s : i8
  sea.assert %ok
  sea.halt
}

// Negations of an assumed value are false: xor %c, 1 and icmp eq %c, 0
// (forward); an assumed negation fixes its operand (backward: assume
// !%d makes %d false); an assumed `and` fixes its conjuncts. The final
// assert of %c becomes assert 1 (asserts-held).
// CHECK-LABEL: sea.program @negated
// CHECK-SAME: "asserts-held" = 1 : i64
// CHECK-SAME: branches = 4 : i64
// CHECK-DAG: %[[F:.*]] = sea.const false
// CHECK-DAG: %[[T:.*]] = sea.const true
// CHECK: sea.cond_br %[[F]], ^bb1, ^bb2
// CHECK: sea.cond_br %[[F]], ^bb3, ^bb4
// CHECK: sea.cond_br %[[F]], ^bb5, ^bb6
// CHECK: sea.cond_br %[[T]], ^bb7, ^bb8
sea.program @negated {
  %t = sea.const 1 : i1
  %f = sea.const 0 : i1
  %c = sea.nondet "c" [0] : i1
  %d = sea.nondet "d" [1] : i1
  %e = sea.nondet "e" [2] : i1
  %nc = sea.xor %c, %t : i1
  %zc = sea.icmp eq, %c, %f : i1
  %nd = sea.xor %t, %d : i1
  %ce = sea.and %c, %e : i1
  %all = sea.and %ce, %nd : i1
  sea.assume %all
  sea.cond_br %nc, ^bb1, ^bb2
^bb1:
  sea.br ^bb2
^bb2:
  sea.cond_br %zc, ^bb3, ^bb4
^bb3:
  sea.br ^bb4
^bb4:
  sea.cond_br %d, ^bb5, ^bb6
^bb5:
  sea.br ^bb6
^bb6:
  sea.cond_br %e, ^bb7, ^bb8
^bb7:
  sea.br ^bb8
^bb8:
  sea.assert %c
  sea.halt
}

// Not dominated: a branch before the assume, and a branch after the join
// of a sibling that assumes %c. Nothing folds (no counters); the assume's
// own operand stays.
// CHECK-LABEL: sea.program @not_dominated
// CHECK-NOT: sea.assume_fold
// CHECK: sea.cond_br %[[C:[^,]*]], ^bb1, ^bb2
// CHECK: sea.assume %[[C]]
// CHECK: sea.cond_br %[[C]], ^bb4, ^bb5
sea.program @not_dominated {
  %c = sea.nondet "c" [0] : i1
  %x = sea.nondet "x" [1] : i1
  sea.cond_br %c, ^bb1, ^bb2
^bb1:
  sea.assume %c
  sea.br ^bb3
^bb2:
  sea.br ^bb3
^bb3:
  sea.cond_br %c, ^bb4, ^bb5
^bb4:
  sea.br ^bb5
^bb5:
  sea.assert %x
  sea.halt
}

// An assert is an assume for the code it dominates: after `assert %c`, a
// branch on %c and a second assert of %c are constant; the first assert's
// own condition stays; a later `assert !%c` fails (asserts-failed).
// CHECK-LABEL: sea.program @by_assert
// CHECK-SAME: "asserts-failed" = 1 : i64, "asserts-held" = 1 : i64
// CHECK-SAME: "by-assert" = 4 : i64, "by-assume" = 0 : i64
// CHECK-DAG: %[[F:.*]] = sea.const false
// CHECK-DAG: %[[T:.*]] = sea.const true
// CHECK: sea.assert %[[C:[^ ]*]]{{$}}
// CHECK: sea.cond_br %[[T]], ^bb1, ^bb2
// CHECK: sea.assert %[[T]]
// CHECK: sea.assert %[[F]]
sea.program @by_assert {
  %t = sea.const 1 : i1
  %c = sea.nondet "c" [0] : i1
  sea.assert %c
  sea.cond_br %c, ^bb1, ^bb2
^bb1:
  sea.br ^bb2
^bb2:
  sea.assert %c
  %nc = sea.xor %c, %t : i1
  sea.assert %nc
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
