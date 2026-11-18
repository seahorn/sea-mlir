// The difference decider of the Ackermann lowering (docs/design/Ackermann.md,
// "The difference decider"): a parser cursor o_{k+1} = o_k + zext(x_k) + 2
// reads p + o_k; any two cursor positions differ by 2..257 per step, so
// their reads are never at one index (interval of the difference) and get no
// constraint. The read at p + 200 can meet a cursor position: those pairs
// keep theirs (p + 0 against p + 200: two numerals, never a constraint).
// The verdict is the same with and without the decider.
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --ack-decider=false 2>&1 | FileCheck %s --check-prefix=OFF
// CHECK: vc.ackermann-reads: 4
// CHECK: vc.ackermann-pairs: 6
// CHECK: vc.ackermann-decider-pairs: 3
// CHECK: vc.ackermann-constraints: 2
// CHECK: result: unsat
// OFF: vc.ackermann-reads: 4
// OFF: vc.ackermann-pairs: 6
// OFF-NOT: vc.ackermann-decider
// OFF: vc.ackermann-constraints: 5
// OFF: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @cursor {
  %m = sea.mem.nondet "m" [0] : !m
  %c4096 = sea.const 4096 : i64
  %p = sea.malloc %c4096 align 8 [1] : i64 -> !sea.ptr<64>
  %c2 = sea.const 2 : i64
  %o0 = sea.const 0 : i64
  %q0 = sea.ptr.add %p, %o0 : !sea.ptr<64>, i64
  %x0 = sea.load %m[%q0] : !m -> i8
  %z1 = sea.zext %x0 : i8 to i64
  %s1 = sea.add %o0, %z1 : i64
  %o1 = sea.add %s1, %c2 : i64
  %q1 = sea.ptr.add %p, %o1 : !sea.ptr<64>, i64
  %x1 = sea.load %m[%q1] : !m -> i8
  %z2 = sea.zext %x1 : i8 to i64
  %s2 = sea.add %o1, %z2 : i64
  %o2 = sea.add %s2, %c2 : i64
  %q2 = sea.ptr.add %p, %o2 : !sea.ptr<64>, i64
  %x2 = sea.load %m[%q2] : !m -> i8
  %c200 = sea.const 200 : i64
  %qf = sea.ptr.add %p, %c200 : !sea.ptr<64>, i64
  %xf = sea.load %m[%qf] : !m -> i8
  %y0 = sea.load %m[%p] : !m -> i8
  %e0 = sea.icmp eq, %x0, %y0 : i8
  %e1 = sea.icmp eq, %x1, %x1 : i8
  %e2 = sea.icmp ule, %x2, %xf : i8
  %e3 = sea.icmp ugt, %x2, %xf : i8
  %e23 = sea.or %e2, %e3 : i1
  %e01 = sea.and %e0, %e1 : i1
  %ok = sea.and %e01, %e23 : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
