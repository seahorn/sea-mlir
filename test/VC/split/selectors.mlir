// @<file>:<line>[:<col>] selectors resolve by kind preference
// (docs/design/Split.md): --split-leaves takes address op results first,
// then address block arguments, then integer op results, then integer
// block arguments; --split takes i1 op results before i1 block arguments.
// Only the first non-empty class can be ambiguous; :<col> picks within it.
//
// Line 768: an address select and an i8 load. The address is the target,
// as before integers were targets.
// RUN: sea-vc %s --lower --program=mixed --seam --split-leaves=@sel.cc:768 | FileCheck %s --check-prefix=ADDR
// ADDR: split.part: 0 choice=%q=leaf:obj.heap.1/paths=1#0/2
// ADDR: split.verdict: unsat parts=2
// Line 769: integers only: the integer select is the target.
// RUN: sea-vc %s --lower --program=mixed --seam --split-leaves=@sel.cc:769 | FileCheck %s --check-prefix=INT
// INT: split.check: level=0 target=%s cases=2 exhaustive=ok exact=ok
// INT: split.verdict: unsat parts=2
// Line 770: two integer selects: ambiguous, listing both with their
// columns; :<col> picks one.
// RUN: not sea-vc %s --lower --program=mixed --seam --split-leaves=@sel.cc:770 2>&1 | FileCheck %s --check-prefix=AMB
// AMB: is ambiguous (2 matches
// AMB-DAG: %t1 sea.select i8 col=3
// AMB-DAG: %t2 sea.select i8 col=20
// AMB: [vc.split-target]
// RUN: sea-vc %s --lower --program=mixed --seam --split-leaves=@sel.cc:770:20 | FileCheck %s --check-prefix=COL
// COL: split.check: level=0 target=%t2 cases=2
// Line 771: an i1 op result and an i1 block argument: --split takes the
// op result.
// RUN: sea-vc %s --lower --program=mixed --seam --split=@sel.cc:771 | FileCheck %s --check-prefix=BOOL
// BOOL: split.check: level=0 target=%e cases=2
// A malformed column.
// RUN: not sea-vc %s --lower --program=mixed --seam --split-leaves=@sel.cc:770:x 2>&1 | FileCheck %s --check-prefix=BAD
// BAD: want @<file>:<line>[:<col>]

!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @mixed {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c8 align 8 [3] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %c = sea.nondet "c" [5] : i1
  %x = sea.nondet "x" [6] : i8
  %p = sea.load %m0[%C] : !m -> !sea.ptr<64>
  %q = sea.select %c, %A, %p : !sea.ptr<64> loc("q"("src/sel.cc":768:3))
  %y = sea.load %m0[%A] : !m -> i8 loc("src/sel.cc":768:9)
  %v = sea.const 7 : i8
  %s = sea.select %c, %x, %v : i8 loc("s"("src/sel.cc":769:3))
  %t1 = sea.select %c, %s, %x : i8 loc("t1"("src/sel.cc":770:3))
  %t2 = sea.select %c, %v, %s : i8 loc("t2"("src/sel.cc":770:20))
  %e = sea.icmp eq, %t1, %t2 : i8 loc("e"("src/sel.cc":771:3))
  sea.cond_br %e, ^a, ^b
^a:
  sea.br ^j(%e : i1)
^b:
  sea.br ^j(%c : i1)
^j(%f : i1 loc("src/sel.cc":771:9)):
  %m1 = sea.store %m0[%q], %v inplace : !m, i8
  %r = sea.load %m1[%A] : !m -> i8
  %e1 = sea.icmp eq, %r, %v : i8
  %e2 = sea.icmp eq, %r, %y : i8
  %ok0 = sea.or %e1, %e2 : i1
  %ok = sea.or %ok0, %f : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
