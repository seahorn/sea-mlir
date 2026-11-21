// RUN: sea-vc %s --program=idle --lower --seam --ack-stride 2>&1 | FileCheck %s --check-prefix=IDLE
// RUN: sea-vc %s --program=mixed --lower --seam --ack-stride 2>&1 | FileCheck %s --check-prefix=MIXED
// RUN: sea-vc %s --program=three --lower --seam --ack-stride 2>&1 | FileCheck %s --check-prefix=THREE
// RUN: sea-vc %s --program=own --ack-own --lower --seam --ack-stride 2>&1 | FileCheck %s --check-prefix=OWN
// The stride rule's model check counts a dropped pair as broken only when
// both reads execute in the model (docs/design/PtrIndex.md section 5): a
// GEP promise is a fact only at an access the run performs. In both
// programs the block ^rd never executes (k < 3, so k > 10 is false), and
// the program assumes the two addresses equal: the reads in ^rd would be
// at one index only by a wrapping offset, which their unexecuted GEPs may
// have. The assumes in ^rd make the solver give those reads different
// cells. That is the solver's free choice (z3 at the pinned version picks
// different cells; another solver may not): the `stride-idle: 1` lines
// show that the path is exercised and depend on that choice.
//
// @idle: both reads of the pair are in ^rd. Neither executes: the pair is
// idle, not broken, and the answer is sat with no rerun.
// IDLE: vc.ackermann-stride-idle: 1
// IDLE-NOT: vc.ackermann-stride-broken
// IDLE-NOT: vc.ackermann-stride: fallback
// IDLE: result: sat
// IDLE: replay: assert-failed
// IDLE: validate: checked={{[0-9]+}} mismatches=0
//
// @mixed: one read executes (entry, the failing path needs it to be 7),
// the other is in ^rd (5 there). The array's cell at that index is the
// executed read's: the replay reads 7 and fails the assertion (with the
// unexecuted read's 5 it would not fail: vc.replay-no-failure).
// MIXED: vc.ackermann-stride-idle: 1
// MIXED-NOT: vc.ackermann-stride-broken
// MIXED-NOT: vc.ackermann-stride: fallback
// MIXED: result: sat
// MIXED: replay: assert-failed
// MIXED: validate: checked={{[0-9]+}} mismatches=0
// MIXED-NOT: vc.replay-no-failure
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @idle {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %k = sea.nondet "k" [4] : i8
  %c3 = sea.const 3 : i8
  %c10 = sea.const 10 : i8
  %lo = sea.icmp ult, %k, %c3 : i8
  sea.assume %lo
  %c = sea.icmp ugt, %k, %c10 : i8
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %ai = sea.ptr.to_int %a : !sea.ptr<64> to i64
  %bi = sea.ptr.to_int %b : !sea.ptr<64> to i64
  %same = sea.icmp eq, %ai, %bi : i64
  sea.assume %same
  sea.cond_br %c, ^rd, ^skip
^rd:
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %c5 = sea.const 5 : i8
  %c7 = sea.const 7 : i8
  %d = sea.icmp eq, %x, %c5 : i8
  sea.assume %d
  %e = sea.icmp eq, %y, %c7 : i8
  sea.assume %e
  %f = sea.const 0 : i1
  sea.br ^join(%f : i1)
^skip:
  %t = sea.const 0 : i1
  sea.br ^join(%t : i1)
^join(%ok : i1):
  sea.assert %ok
  sea.halt
}
sea.program @mixed {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %k = sea.nondet "k" [4] : i8
  %c3 = sea.const 3 : i8
  %c10 = sea.const 10 : i8
  %lo = sea.icmp ult, %k, %c3 : i8
  sea.assume %lo
  %c = sea.icmp ugt, %k, %c10 : i8
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %ai = sea.ptr.to_int %a : !sea.ptr<64> to i64
  %bi = sea.ptr.to_int %b : !sea.ptr<64> to i64
  %same = sea.icmp eq, %ai, %bi : i64
  sea.assume %same
  %y = sea.load %m0[%a] : !pm -> i8
  %c7 = sea.const 7 : i8
  %ok0 = sea.icmp ne, %y, %c7 : i8
  sea.cond_br %c, ^rd, ^skip
^rd:
  %x = sea.load %m0[%b] : !pm -> i8
  %c5 = sea.const 5 : i8
  %d = sea.icmp eq, %x, %c5 : i8
  sea.assume %d
  %t1 = sea.const 1 : i1
  sea.br ^join(%t1 : i1)
^skip:
  sea.br ^join(%ok0 : i1)
^join(%ok : i1):
  sea.assert %ok
  sea.halt
}

// @three: a read in ^rd (does not execute) comes first, then two executed
// reads in ^join at the same index (5 and 7). The executed pair breaks a
// promise: it must be compared directly, not only each read with the
// unexecuted one. The model is rejected, and the run without the rule
// answers unsat (one cell cannot be both 5 and 7).
// THREE: vc.ackermann-stride-broken: 1
// THREE: vc.ackermann-stride: fallback
// THREE: result: unsat
sea.program @three {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %kk = sea.nondet "kk" [5] : i64
  %k = sea.nondet "k" [4] : i8
  %c3 = sea.const 3 : i8
  %c10 = sea.const 10 : i8
  %lo = sea.icmp ult, %k, %c3 : i8
  sea.assume %lo
  %c = sea.icmp ugt, %k, %c10 : i8
  %a = sea.ptr.index %p [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %u = sea.ptr.index %p [%kk] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %ai = sea.ptr.to_int %a : !sea.ptr<64> to i64
  %bi = sea.ptr.to_int %b : !sea.ptr<64> to i64
  %ui = sea.ptr.to_int %u : !sea.ptr<64> to i64
  %same = sea.icmp eq, %ai, %bi : i64
  sea.assume %same
  %same2 = sea.icmp eq, %ai, %ui : i64
  sea.assume %same2
  sea.cond_br %c, ^rd, ^skip
^rd:
  %z = sea.load %m0[%u] : !pm -> i8
  %c9 = sea.const 9 : i8
  %dz = sea.icmp eq, %z, %c9 : i8
  sea.assume %dz
  sea.br ^join
^skip:
  sea.br ^join
^join:
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %c5 = sea.const 5 : i8
  %c7 = sea.const 7 : i8
  %d = sea.icmp eq, %x, %c5 : i8
  sea.assume %d
  %e = sea.icmp eq, %y, %c7 : i8
  sea.assume %e
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// @own, with --ack-own: reads of two objects p and q, assumed at one
// address (a model that breaks A-INB / A-OWN), so the ownership drop drops
// the pair. The read of q is in ^rd and does not execute; the read of p
// executes and the failing path needs it to be 5. The array keeps the
// executed read's cell, so the replay reads 5, fails the assertion and
// validates: the collision is excused (with the cell of the read in ^rd,
// 7, the run would not fail).
// OWN: own-excused=1
// OWN: replay: assert-failed
// OWN: validate: checked={{[0-9]+}} mismatches=0
// OWN-NOT: vc.replay-no-failure
sea.program @own {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 16 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %q = sea.malloc %n align 8 [6] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %k = sea.nondet "k" [4] : i8
  %c3 = sea.const 3 : i8
  %c10 = sea.const 10 : i8
  %lo = sea.icmp ult, %k, %c3 : i8
  sea.assume %lo
  %c = sea.icmp ugt, %k, %c10 : i8
  %a = sea.ptr.index %p [%i] strides [1] offset 0 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %q [%j] strides [1] offset 0 inbounds : !sea.ptr<64>, i64
  %ai = sea.ptr.to_int %a : !sea.ptr<64> to i64
  %bi = sea.ptr.to_int %b : !sea.ptr<64> to i64
  %same = sea.icmp eq, %ai, %bi : i64
  sea.assume %same
  %x = sea.load %m0[%a] : !pm -> i8
  %c5 = sea.const 5 : i8
  %ok0 = sea.icmp ne, %x, %c5 : i8
  sea.cond_br %c, ^rd, ^skip
^rd:
  %y = sea.load %m0[%b] : !pm -> i8
  %c7 = sea.const 7 : i8
  %d = sea.icmp eq, %y, %c7 : i8
  sea.assume %d
  %t1 = sea.const 1 : i1
  sea.br ^join(%t1 : i1)
^skip:
  sea.br ^join(%ok0 : i1)
^join(%ok : i1):
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
