// The fold of --vc-fold-top-eqs and a concat fact with a part that mentions
// an object (docs/design/FoldTopEqs.md, "Pointer observations"): the concat
// is skipped, its other parts are facts, substituted elsewhere and kept in
// their own conjunct. (--lower-disable=assume-fold: sea-assume-fold would
// replace the uses before the VC.)
// RUN: sea-vc %s --program=top --lower --lower-disable=assume-fold --seam | FileCheck %s --check-prefix=RES
// RUN: sea-vc %s --program=top --lower --lower-disable=assume-fold --seam --smt2 2>/dev/null | FileCheck %s --check-prefix=TOP
// RUN: sea-vc %s --program=nested --lower --lower-disable=assume-fold --seam | FileCheck %s --check-prefix=RES
// RUN: sea-vc %s --program=nested --lower --lower-disable=assume-fold --seam --smt2 2>/dev/null | FileCheck %s --check-prefix=NESTED
//
// @top: concat(x + y, to_int(a)) = 0x7_00001000 fixes x + y = 7; with
// x = y (2x = 7 has no solution) the assumptions are unsat. The use of
// x + y in the other assume becomes 7; the fact stays in its conjunct.
// RES: solver-answer: unsat
// RES: result: unsat
// TOP: (= (concat (bvadd nd.0_0 nd.1_0) #x00001000) #x0000000700001000)
// TOP: (bvult #x00000007 #x00000064)
//
// @nested: concat(to_int(a), concat(x, x + y)) = 0x1000_00000003_00000007
// fixes x = 3 and x + y = 7 (y = 4); y = 5 contradicts it. The inner
// concat stays in the outer one (y := 5 is eliminated after the fold).
// NESTED: (concat #x00001000 (concat nd.0_0 (bvadd nd.0_0
sea.program @top {
^bb0:
  %a = sea.segment "heap.1" base 4096 : i32 size 16 : i32 align 16 kind "heap" : !sea.addr<32>
  %ai = sea.addr.to_int %a : !sea.addr<32> to i32
  %x = sea.nondet "x" [0] : i32
  %y = sea.nondet "y" [1] : i32
  %s = sea.add %x, %y : i32
  %v = sea.concat %s, %ai : i32, i32 -> i64
  %k = sea.const 30064775168 : i64
  %c = sea.icmp eq, %v, %k : i64
  sea.assume %c
  %h = sea.const 100 : i32
  %u = sea.icmp ult, %s, %h : i32
  sea.assume %u
  %e = sea.icmp eq, %x, %y : i32
  sea.assume %e
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}
sea.program @nested {
^bb0:
  %a = sea.segment "heap.1" base 4096 : i32 size 16 : i32 align 16 kind "heap" : !sea.addr<32>
  %ai = sea.addr.to_int %a : !sea.addr<32> to i32
  %x = sea.nondet "x" [0] : i32
  %y = sea.nondet "y" [1] : i32
  %s = sea.add %x, %y : i32
  %in = sea.concat %x, %s : i32, i32 -> i64
  %v = sea.concat %ai, %in : i32, i64 -> i96
  %k = sea.const 75557863725927208321031 : i96
  %c = sea.icmp eq, %v, %k : i96
  sea.assume %c
  %h = sea.const 100 : i32
  %u = sea.icmp ult, %s, %h : i32
  sea.assume %u
  %five = sea.const 5 : i32
  %e = sea.icmp eq, %y, %five : i32
  sea.assume %e
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
