// RUN: sea-opt %s --sea-assume-fold | FileCheck %s
// sea-assume-fold (docs/design/AssumeFold.md): after `sea.assume %c` or
// `sea.assert %c`, the uses of %c (and of what %c fixes) in ops the check
// dominates read a constant. Branches and asserts stay: canonicalize and
// sea-merge-asserts fold them. One program per property; the counters are
// on the program.

// The reduced shape: assume %c, then cond_br %c and a select on %c.
// CHECK-LABEL: sea.program @direct
// CHECK-SAME: sea.assume_fold = {"asserts-failed" = 0 : i64, "asserts-held" = 0 : i64, blocks = 1 : i64, branches = 1 : i64, "by-assert" = 0 : i64, "by-assume" = 2 : i64, "by-edge" = 0 : i64, selects = 1 : i64, uses = 2 : i64, values = 1 : i64}
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

// Not dominated: a select before the assume, and a branch after the join
// of a sibling that assumes %c. Nothing folds (no counters); the assume's
// own operand stays.
// CHECK-LABEL: sea.program @not_dominated
// CHECK-NOT: sea.assume_fold
// CHECK: sea.select %[[C:[^,]*]],
// CHECK: sea.assume %[[C]]
// CHECK: sea.cond_br %[[C]], ^bb4, ^bb5
sea.program @not_dominated {
  %c = sea.nondet "c" [0] : i1
  %x = sea.nondet "x" [1] : i1
  %d = sea.nondet "d" [2] : i1
  %s = sea.select %c, %x, %d : i1
  sea.cond_br %d, ^bb1, ^bb2
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
  sea.assert %s
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


// Edge facts: on the true edge of `cond_br %c` %c is 1, on the false edge
// 0, in the block the edge enters alone and every block it dominates. The
// reduced shape: a second branch on %c under the true edge, a select on
// %c under the false edge.
// CHECK-LABEL: sea.program @edge
// CHECK-SAME: "by-edge" = 2 : i64
// CHECK-SAME: selects = 1 : i64
// CHECK-DAG: %[[F:.*]] = sea.const false
// CHECK-DAG: %[[T:.*]] = sea.const true
// CHECK: sea.cond_br %[[C:[^,]*]], ^bb1, ^bb4
// CHECK: ^bb1:
// CHECK-NEXT: sea.cond_br %[[T]], ^bb2, ^bb3
// CHECK: ^bb4:
// CHECK-NEXT: sea.select %[[F]],
sea.program @edge {
  %t = sea.const 1 : i1
  %c5 = sea.const 5 : i8
  %x = sea.nondet "x" [0] : i8
  %y = sea.nondet "y" [1] : i8
  %c = sea.icmp ult, %x, %c5 : i8
  sea.cond_br %c, ^a, ^b
^a:
  sea.cond_br %c, ^a1, ^a2
^a1:
  sea.br ^j(%x : i8)
^a2:
  sea.br ^j(%y : i8)
^b:
  %s = sea.select %c, %x, %y : i8
  sea.br ^j(%s : i8)
^j(%r : i8):
  %e = sea.icmp ne, %r, %c5 : i8
  sea.assert %e
  sea.halt
}

// A block with several incoming edges: a fact holds there only if it
// holds on every edge. ^bb3 is entered on the true edge of `cond_br %c`
// (%c = 1) and on the true edge of `cond_br %d` in ^bb1, where %c = 0
// (the false edge into ^bb1): nothing about %c or %d holds in ^bb3. ^bb4
// is entered by both edges of one cond_br on %e: no fact. Nothing folds.
// CHECK-LABEL: sea.program @join_no_fold
// CHECK-NOT: sea.assume_fold
// CHECK: %[[C:.*]] = sea.nondet "c"
// CHECK: %[[D:.*]] = sea.nondet "d"
// CHECK: %[[E:.*]] = sea.nondet "e"
// CHECK: ^bb3:
// CHECK-NEXT: sea.or %[[C]], %[[D]] : i1
// CHECK: ^bb4(%{{.*}}: i1):
// CHECK-NEXT: sea.and %{{[^,]*}}, %[[E]] : i1
sea.program @join_no_fold {
  %c = sea.nondet "c" [0] : i1
  %d = sea.nondet "d" [1] : i1
  %e = sea.nondet "e" [2] : i1
  %x = sea.nondet "x" [3] : i1
  sea.cond_br %c, ^bb3, ^bb1
^bb1:
  sea.cond_br %d, ^bb3, ^bb2
^bb2:
  sea.br ^bb3
^bb3:
  %cd = sea.or %c, %d : i1
  %q = sea.xor %cd, %x : i1
  sea.cond_br %e, ^bb4(%q : i1), ^bb4(%x : i1)
^bb4(%r : i1):
  %w = sea.and %r, %e : i1
  sea.assert %w
  sea.halt
}

// Several incoming edges that agree: ^bb2 is entered on the true edge of
// `cond_br %c` and on the true edge of `cond_br %d`; `or %c, %d` is 1 on
// both, so in ^bb2; %c and %d alone are not.
// CHECK-LABEL: sea.program @join_agree
// CHECK-SAME: "by-edge" = 1 : i64
// CHECK: %[[T:.*]] = sea.const true
// CHECK: ^bb2:
// CHECK-NEXT: sea.assert %[[T]]
// CHECK: ^bb3:
// CHECK-NEXT: sea.assert %{{[^ ]*}}{{$}}
sea.program @join_agree {
  %c = sea.nondet "c" [0] : i1
  %d = sea.nondet "d" [1] : i1
  %o = sea.or %c, %d : i1
  sea.cond_br %c, ^bb2, ^bb1
^bb1:
  sea.cond_br %d, ^bb2, ^bb3
^bb2:
  sea.assert %o
  sea.halt
^bb3:
  sea.assert %o
  sea.halt
}


// Select form: `select %c, %x, false` is `and %c, %x`, `select %w, true,
// %c` is `or %w, %c`. The assumed conjunction fixes %c and %x (backward);
// %c true fixes the disjunction (forward), so the final assert holds.
// CHECK-LABEL: sea.program @select_form
// CHECK-SAME: "asserts-held" = 1 : i64
// CHECK-SAME: branches = 1 : i64
// CHECK: %[[T:.*]] = sea.const true
// CHECK: sea.cond_br %[[T]], ^bb1, ^bb2
// CHECK: sea.assert %[[T]]
sea.program @select_form {
  %t = sea.const 1 : i1
  %f = sea.const 0 : i1
  %c = sea.nondet "c" [0] : i1
  %x = sea.nondet "x" [1] : i1
  %w = sea.nondet "w" [2] : i1
  %a = sea.select %c, %x, %f : i1
  %o = sea.select %w, %t, %c : i1
  sea.assume %a
  sea.cond_br %x, ^bb1, ^bb2
^bb1:
  sea.br ^bb2
^bb2:
  sea.assert %o
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
