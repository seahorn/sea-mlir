// RUN: sea-opt %s --sea-range-fold | FileCheck %s
// RUN: sea-opt %s --sea-range-fold=mode=check | FileCheck %s --check-prefix=CHK
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-range-fold{max-depth=3}))' | FileCheck %s --check-prefix=SEQ
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-range-fold{mode=check max-depth=3}))' | FileCheck %s --check-prefix=SEQ
// sea-range-fold (docs/design/RangeFold.md): one program per property; each
// folds its `%ok` (the counters on the program: icmp = 1) or must not
// (icmp = 0). The ranges come from InferIntRangeInterface, intersected with
// the simple bounds of dominating assumes.

// Transfer functions. x in [0, 5] (a conjunction of two bounds):
// x + 10 < 16 unsigned; x - 3 is in [-3, 2] signed (folds) but wraps
// unsigned (ult 3 does not).
// CHECK-LABEL: sea.program @add_sub
// CHECK-SAME: icmp = 2 : i64
// CHECK: sea.icmp ult, %{{.*}}, %{{.*}} : i8
sea.program @add_sub {
  %c0 = sea.const 0 : i8
  %c3 = sea.const 3 : i8
  %c6 = sea.const 6 : i8
  %c10 = sea.const 10 : i8
  %c16 = sea.const 16 : i8
  %x = sea.nondet "x" [0] : i8
  %a = sea.icmp sge, %x, %c0 : i8
  %b = sea.icmp slt, %x, %c6 : i8
  %ab = sea.and %a, %b : i1
  sea.assume %ab
  %y = sea.add %x, %c10 : i8
  %ok1 = sea.icmp ult, %y, %c16 : i8
  %z = sea.sub %x, %c3 : i8
  %ok2 = sea.icmp slt, %z, %c3 : i8
  %no = sea.icmp ult, %z, %c3 : i8
  %ok12 = sea.and %ok1, %ok2 : i1
  %ok = sea.and %ok12, %no : i1
  sea.assert %ok
  sea.halt
}

// Overflow widens: x in [100, 120] u: 2x in [200, 240] (folds: uge 200);
// x in [100, 130] u: 2x may wrap (260 = 4 mod 256): full range, no fold.
// CHECK-LABEL: sea.program @mul_wrap
// CHECK-SAME: icmp = 1 : i64
// CHECK: sea.icmp uge, %[[W:.*]], %{{.*}} : i8
// CHECK: sea.assert
sea.program @mul_wrap {
  %c2 = sea.const 2 : i8
  %c100 = sea.const 100 : i8
  %c121 = sea.const 121 : i8
  %c131 = sea.const 131 : i8
  %c200 = sea.const 200 : i8
  %x = sea.nondet "x" [0] : i8
  %w = sea.nondet "w" [1] : i8
  %a = sea.icmp uge, %x, %c100 : i8
  %b = sea.icmp ult, %x, %c121 : i8
  %c = sea.icmp uge, %w, %c100 : i8
  %d = sea.icmp ult, %w, %c131 : i8
  %ab = sea.and %a, %b : i1
  %cd = sea.and %c, %d : i1
  %abcd = sea.and %ab, %cd : i1
  sea.assume %abcd
  %y = sea.mul %x, %c2 : i8
  %ok1 = sea.icmp uge, %y, %c200 : i8
  %v = sea.mul %w, %c2 : i8
  %no = sea.icmp uge, %v, %c200 : i8
  %ok = sea.and %ok1, %no : i1
  sea.assert %ok
  sea.halt
}

// Casts, bit ops and shifts: x < 10 (u, i8).
// CHECK-LABEL: sea.program @casts_bits
// CHECK-SAME: icmp = 6 : i64
// CHECK: sea.assume
// CHECK-NOT: sea.icmp
// CHECK: sea.halt
sea.program @casts_bits {
  %c1 = sea.const 1 : i8
  %c2 = sea.const 2 : i8
  %c7 = sea.const 7 : i8
  %c10 = sea.const 10 : i8
  %c40 = sea.const 40 : i8
  %c5 = sea.const 5 : i8
  %c10w = sea.const 10 : i32
  %c10n = sea.const 10 : i4
  %x = sea.nondet "x" [0] : i8
  %a = sea.icmp ult, %x, %c10 : i8
  sea.assume %a
  %z = sea.zext %x : i8 to i32
  %ok1 = sea.icmp ult, %z, %c10w : i32
  %s = sea.sext %x : i8 to i32
  %ok2 = sea.icmp slt, %s, %c10w : i32
  %t = sea.trunc %z : i32 to i4
  %ok3 = sea.icmp ult, %t, %c10n : i4
  %m = sea.and %x, %c7 : i8
  %ok4 = sea.icmp ule, %m, %c7 : i8
  %r = sea.lshr %x, %c1 : i8
  %ok5 = sea.icmp ult, %r, %c5 : i8
  %l = sea.shl %x, %c2 : i8
  %ok6 = sea.icmp ult, %l, %c40 : i8
  %o1 = sea.and %ok1, %ok2 : i1
  %o2 = sea.and %ok3, %ok4 : i1
  %o3 = sea.and %ok5, %ok6 : i1
  %o12 = sea.and %o1, %o2 : i1
  %ok = sea.and %o12, %o3 : i1
  sea.assert %ok
  sea.halt
}

// udiv: x / d with d possibly 0 is ~0 (bvudiv): no fold; with d >= 1 it is
// at most x (folds).
// CHECK-LABEL: sea.program @udiv
// CHECK-SAME: icmp = 1 : i64
// CHECK: %[[Q:.*]] = sea.udiv
// CHECK: sea.icmp ult, %[[Q]]
sea.program @udiv {
  %c1 = sea.const 1 : i8
  %c100 = sea.const 100 : i8
  %x = sea.nondet "x" [0] : i8
  %d = sea.nondet "d" [1] : i8
  %e = sea.nondet "e" [2] : i8
  %a = sea.icmp ult, %x, %c100 : i8
  %b = sea.icmp uge, %e, %c1 : i8
  %ab = sea.and %a, %b : i1
  sea.assume %ab
  %q = sea.udiv %x, %d : i8
  %no = sea.icmp ult, %q, %c100 : i8
  %p = sea.udiv %x, %e : i8
  %ok1 = sea.icmp ult, %p, %c100 : i8
  %ok = sea.and %no, %ok1 : i1
  sea.assert %ok
  sea.halt
}

// A guarded store destination under a bound assumption: the row of a slot in
// [0, T) is in range, so the select takes its row, not the sink.
// CHECK-LABEL: sea.program @select_row
// CHECK-SAME: select = 1 : i64
// CHECK-NOT: sea.select
// CHECK: sea.halt
sea.program @select_row {
  %c0 = sea.const 0 : i64
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c6 = sea.const 6 : i64
  %c12 = sea.const 12 : i64
  %s = sea.nondet "slot" [0] : i32
  %x = sea.sext %s : i32 to i64
  %a = sea.icmp sge, %x, %c0 : i64
  %b = sea.icmp slt, %x, %c6 : i64
  %ab = sea.and %a, %b : i1
  sea.assume %ab
  %m = sea.mul %x, %c2 : i64
  %lo = sea.add %c0, %m : i64
  %g1 = sea.icmp sge, %lo, %c0 : i64
  %hi = sea.add %lo, %c1 : i64
  %g2 = sea.icmp slt, %hi, %c12 : i64
  %in = sea.and %g1, %g2 : i1
  %sel = sea.select %in, %lo, %c12 : i64
  %ok = sea.icmp ult, %sel, %c12 : i64
  sea.assert %ok
  sea.halt
}

// Dominance: the assume in a sibling branch does not dominate the join.
// CHECK-LABEL: sea.program @sibling
// CHECK-SAME: icmp = 0 : i64
// CHECK: sea.br ^[[J:bb[0-9]+]]
// CHECK: sea.br ^[[J]]
// CHECK: ^[[J]]:
// CHECK-NEXT: sea.icmp ult
sea.program @sibling {
  %c6 = sea.const 6 : i8
  %x = sea.nondet "x" [0] : i8
  %c = sea.nondet "c" [1] : i1
  sea.cond_br %c, ^a, ^b
^a:
  %a = sea.icmp ult, %x, %c6 : i8
  sea.assume %a
  sea.br ^j
^b:
  sea.br ^j
^j:
  %ok = sea.icmp ult, %x, %c6 : i8
  sea.assert %ok
  sea.halt
}

// Dominance across blocks: the comparison is in the entry, the assume in one
// arm, a use of the comparison after the assume there: the fold point is the
// comparison, which the assume does not dominate.
// CHECK-LABEL: sea.program @def_before_arm
// CHECK-SAME: icmp = 0 : i64
// CHECK: %[[OK:.*]] = sea.icmp ult
// CHECK: sea.assert %[[OK]]
sea.program @def_before_arm {
  %c6 = sea.const 6 : i8
  %x = sea.nondet "x" [0] : i8
  %c = sea.nondet "c" [1] : i1
  %ok = sea.icmp ult, %x, %c6 : i8
  sea.cond_br %c, ^a, ^b
^a:
  %a = sea.icmp ult, %x, %c6 : i8
  sea.assume %a
  sea.assert %ok
  sea.halt
^b:
  sea.unreachable
}

// Dominance in a block: an assume after the comparison is no fact for it.
// CHECK-LABEL: sea.program @later
// CHECK-SAME: icmp = 0 : i64
sea.program @later {
  %c6 = sea.const 6 : i8
  %x = sea.nondet "x" [0] : i8
  %ok = sea.icmp ult, %x, %c6 : i8
  %a = sea.icmp ult, %x, %c6 : i8
  sea.assume %a
  sea.assert %ok
  sea.halt
}

// A dominating block's assume is a fact in the blocks below it; the
// constant on the left (sgt 6, x == x < 6); a signed bound inside one sign
// half is an unsigned one too (ult).
// CHECK-LABEL: sea.program @dominating
// CHECK-SAME: icmp = 2 : i64
sea.program @dominating {
  %c0 = sea.const 0 : i8
  %c6 = sea.const 6 : i8
  %x = sea.nondet "x" [0] : i8
  %c = sea.nondet "c" [1] : i1
  %a = sea.icmp sgt, %c6, %x : i8
  %n = sea.icmp sle, %c0, %x : i8
  %an = sea.and %a, %n : i1
  sea.assume %an
  sea.cond_br %c, ^a, ^b
^a:
  %ok1 = sea.icmp ult, %x, %c6 : i8
  sea.assert %ok1
  sea.halt
^b:
  %ok2 = sea.icmp ule, %x, %c6 : i8
  sea.assume %ok2
  sea.halt
}

// Facts: nested conjunctions decompose (x in [2, 4]); an `ne` at the end of
// the range is a bound; a conjunct `true` is nothing; a disjunction and a
// relation of two values are not facts (facts-skipped = 2).
// CHECK-LABEL: sea.program @facts
// CHECK-SAME: facts = 3 : i64, "facts-skipped" = 2 : i64
// CHECK-SAME: icmp = 2 : i64
sea.program @facts {
  %t = sea.const true
  %c0 = sea.const 0 : i8
  %c2 = sea.const 2 : i8
  %c4 = sea.const 4 : i8
  %c5 = sea.const 5 : i8
  %x = sea.nondet "x" [0] : i8
  %y = sea.nondet "y" [1] : i8
  %z = sea.nondet "z" [2] : i8
  %a = sea.icmp uge, %x, %c2 : i8
  %b = sea.icmp ule, %x, %c4 : i8
  %nz = sea.icmp ne, %z, %c0 : i8
  %or = sea.or %a, %b : i1
  %rel = sea.icmp ult, %y, %x : i8
  %ab = sea.and %a, %b : i1
  %tab = sea.and %t, %ab : i1
  %n1 = sea.and %tab, %nz : i1
  %n2 = sea.and %or, %rel : i1
  %all = sea.and %n1, %n2 : i1
  sea.assume %all
  %ok1 = sea.icmp ult, %x, %c5 : i8
  %ok2 = sea.icmp ugt, %z, %c0 : i8
  %ok = sea.and %ok1, %ok2 : i1
  sea.assert %ok
  sea.halt
}

// A bound on sext(x) is one on x (exactly inverted).
// CHECK-LABEL: sea.program @through_ext
// CHECK-SAME: icmp = 1 : i64
// mode=check: the fold becomes an assert of its value, the program's own
// assert is dropped.
// CHK-LABEL: sea.program @through_ext
// CHK: %[[OK:.*]] = sea.icmp ult
// CHK-NEXT: %[[T:.*]] = sea.const true
// CHK-NEXT: %[[EQ:.*]] = sea.icmp eq, %[[OK]], %[[T]] : i1
// CHK-NEXT: sea.assert %[[EQ]]
// CHK-NEXT: sea.halt
sea.program @through_ext {
  %c0 = sea.const 0 : i64
  %c6 = sea.const 6 : i64
  %c6n = sea.const 6 : i32
  %s = sea.nondet "s" [0] : i32
  %x = sea.sext %s : i32 to i64
  %a = sea.icmp sge, %x, %c0 : i64
  %b = sea.icmp slt, %x, %c6 : i64
  %ab = sea.and %a, %b : i1
  sea.assume %ab
  %ok = sea.icmp ult, %s, %c6n : i32
  sea.assert %ok
  sea.halt
}

// Contradictory facts (x <u 0): an empty range, no fold under it (the one
// icmp folded is the assume's own condition, false for every x).
// CHECK-LABEL: sea.program @empty
// CHECK-SAME: empty = 1 : i64
// CHECK-SAME: icmp = 1 : i64
// CHECK: sea.icmp ult, %{{.*}}, %{{.*}} : i8
// CHECK-NEXT: sea.assert
sea.program @empty {
  %c0 = sea.const 0 : i8
  %c6 = sea.const 6 : i8
  %x = sea.nondet "x" [0] : i8
  %a = sea.icmp ult, %x, %c0 : i8
  sea.assume %a
  %ok = sea.icmp ult, %x, %c6 : i8
  sea.assert %ok
  sea.halt
}

// A nuw add: x in [0, 10], x + 250 nuw in [250, 255] by the flag (binding
// semantics; without it, it wraps); the fold is counted (wrap-flags).
// CHECK-LABEL: sea.program @wrap_flags
// CHECK-SAME: icmp = 1 : i64
// CHECK-SAME: "wrap-flags" = 1 : i64
sea.program @wrap_flags {
  %c11 = sea.const 11 : i8
  %c250 = sea.const 250 : i8
  %x = sea.nondet "x" [0] : i8
  %a = sea.icmp ult, %x, %c11 : i8
  sea.assume %a
  %y = sea.add %x, %c250 nuw : i8
  %ok = sea.icmp uge, %y, %c250 : i8
  sea.assert %ok
  sea.halt
}

// Fold and check decide on the unmodified program, so they cover the same
// candidates: with max-depth=3, %ok's walk reaches the assumed x only past
// the cap; folding %b first would hand %ok a constant (two folds against one
// check before the two phases). One fold or check, the comparison %b.
// SEQ-LABEL: sea.program @seq
// SEQ-SAME: bool = 0 : i64, checks = [[N:[01]]] : i64
// SEQ-SAME: icmp = 1 : i64
sea.program @seq {
  %c6 = sea.const 6 : i8
  %c1 = sea.const 1 : i8
  %x = sea.nondet "x" [0] : i8
  %a = sea.icmp ult, %x, %c6 : i8
  sea.assume %a
  %b = sea.icmp ult, %x, %c6 : i8
  %z = sea.zext %b : i1 to i8
  %ok = sea.icmp eq, %z, %c1 : i8
  sea.assert %ok
  sea.halt
}

// mode=check of a select: the assert of its condition comes right before
// it; a tagged assert of the program is dropped, its reach-site marker
// stays.
// CHK-LABEL: sea.program @check_select
// CHK: sea.assume
// CHK-NEXT: %[[IN:.*]] = sea.icmp ult
// CHK-NEXT: sea.const true
// CHK-NEXT: sea.icmp eq, %[[IN]]
// CHK-NEXT: sea.assert
// CHK-NEXT: %[[T:.*]] = sea.const true
// CHK-NEXT: %[[EQ:.*]] = sea.icmp eq, %[[IN]], %[[T]] : i1
// CHK-NEXT: sea.assert %[[EQ]]
// CHK-NEXT: sea.select %[[IN]]
// CHK-NOT: sea.assert {{.*}}reach_site
// CHK: sea.print "sea.assert" {sea.reach_site = 3 : i64}
// CHK-NEXT: sea.halt
sea.program @check_select {
  %c6 = sea.const 6 : i8
  %c9 = sea.const 9 : i8
  %x = sea.nondet "x" [0] : i8
  %y = sea.nondet "y" [1] : i8
  %a = sea.icmp ult, %x, %c6 : i8
  sea.assume %a
  %in = sea.icmp ult, %x, %c6 : i8
  %s = sea.select %in, %x, %y : i8
  %ok = sea.icmp ult, %s, %c9 : i8
  sea.assert %ok {sea.reach_site = 3 : i64}
  sea.halt
}


// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
