// RUN: sea-vc %s --program=held_then_false --lower --replay-original 2>&1 | FileCheck %s --check-prefix=HELD
// RUN: sea-vc %s --program=const_pair --lower --replay-original 2>&1 | FileCheck %s --check-prefix=PAIR
// RUN: sea-vc %s --program=late_const --lower --range-fold=on --replay-original 2>&1 | FileCheck %s --check-prefix=LATE
// RUN: sea-opt %s --sea-merge-asserts='report=false' | FileCheck %s --check-prefix=MERGE
// Which assert a replay names when sea-merge-asserts meets constant
// asserts (MergeAsserts.cpp). An assert of constant 1 is dropped, one of
// constant 0 is a direct branch to its landing block; a run that passes a
// held assert and then fails names the one that failed, so the original
// program (--replay-original) fails at the same assert: match=yes.
// @held_then_false: `assume c; assert c; assert 0`: the run passes the
// first assert's branch and fails at the constant one.
// HELD: replay: assert-op: x.c:9:3
// HELD: replay.original: status=assert-failed at=x.c:9:3 match=yes
// @const_pair: sccp makes the first assert 1 and the second 0.
// PAIR: replay: assert-op: x.c:4:3
// PAIR: replay.original: status=assert-failed at=x.c:4:3 match=yes
// @late_const: both conditions become constant only after the merge
// (sea-range-fold under the assume's bound); canonicalize folds the
// merged branches: the held one loses its marker, the failed one keeps it
// and is reported.
// LATE: [fold.assert-false]
// LATE: replay: assert-op: x.c:4:3
// LATE: replay.original: status=assert-failed at=x.c:4:3 match=yes
// MERGE-LABEL: sea.program @const_pair
// MERGE-NOT: sea.cond_br
// MERGE: sea.br ^[[LAND:bb[0-9]+]]{{$}}
// MERGE-NOT: sea.cond_br
// MERGE: ^[[LAND]]:
// MERGE-NEXT: sea.const false
// MERGE-NOT: sea.cond_br
// MERGE: sea.halt
sea.program @held_then_false {
  %c = sea.nondet "c" [0] : i1
  sea.assume %c loc("h.c":2:3)
  sea.assert %c loc("x.c":3:3)
  %f = sea.const 0 : i1
  sea.assert %f loc("x.c":9:3)
  sea.halt
}

sea.program @const_pair {
  %t = sea.const 1 : i1
  %f = sea.const 0 : i1
  sea.assert %t loc("x.c":3:3)
  sea.assert %f loc("x.c":4:3)
  sea.halt
}

sea.program @late_const {
  %c4 = sea.const 4 : i8
  %c8 = sea.const 8 : i8
  %c10 = sea.const 10 : i8
  %x = sea.nondet "x" [0] : i8
  %b = sea.icmp ult, %x, %c4 : i8
  sea.assume %b loc("h.c":2:3)
  %ok = sea.icmp ult, %x, %c8 : i8
  sea.assert %ok loc("x.c":3:3)
  %bad = sea.icmp ugt, %x, %c10 : i8
  sea.assert %bad loc("x.c":4:3)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
