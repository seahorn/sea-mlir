// sea-vc --arith-stats (tools/sea-vc/ArithStats.h): the multiplications,
// divisions and remainders of the query the solver gets, by operand kind,
// and per source location of the sea op that made them. %x is a load: with
// --seam its products go through the boundary and the Ackermann lowering,
// and keep their locations.
// RUN: sea-vc %s --lower --seam --vcgen-only --arith-stats | FileCheck %s
// RUN: sea-vc %s --lower --vcgen-only --arith-stats | FileCheck %s
// CHECK: arith.total: ops=5 attributed=5 unattributed=0
// CHECK-DAG: arith.op: op=bvmul width=8 operands=numeral count=1 unattributed=0
// CHECK-DAG: arith.op: op=bvmul width=8 operands=ite-num count=1 unattributed=0 conds=2 conds-max=2
// CHECK-DAG: arith.op: op=bvmul width=8 operands=ite-sym count=1 unattributed=0 conds=1 conds-max=1
// CHECK-DAG: arith.op: op=bvmul width=8 operands=nonconst count=1 unattributed=0
// CHECK-DAG: arith.op: op=bvudiv width=8 operands=nonconst count=1 unattributed=0
// CHECK-DAG: arith.loc: op=bvmul width=8 operands=numeral count=1 loc=k.c:2:3
// CHECK-DAG: arith.loc: op=bvmul width=8 operands=ite-num count=1 conds-max=2 loc=k.c:3:3
// CHECK-DAG: arith.loc: op=bvmul width=8 operands=ite-sym count=1 conds-max=1 loc=k.c:4:3
// CHECK-DAG: arith.loc: op=bvmul width=8 operands=nonconst count=1 loc=k.c:5:3
// CHECK-DAG: arith.loc: op=bvudiv width=8 operands=nonconst count=1 loc=k.c:6:3
// CHECK: result: none
// jsonl: one record per line, the location as a field
// RUN: sea-vc %s --lower --seam --vcgen-only --arith-stats --format=jsonl | FileCheck %s --check-prefix=JSON
// JSON: {"kind":"arith-loc","op":"bvmul","width":8,"operands":"nonconst","count":1,"loc":"k.c:5:3",
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @p {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %x = sea.load %m0[%p] : !m -> i8 loc("k.c":1:1)
  %y = sea.nondet "y" [3] : i8
  %c1 = sea.nondet "c1" [4] : i1
  %c2 = sea.nondet "c2" [5] : i1
  %k3 = sea.const 3 : i8
  %k5 = sea.const 5 : i8
  %k7 = sea.const 7 : i8
  %s0 = sea.select %c2, %k3, %k5 : i8
  %s = sea.select %c1, %s0, %k7 : i8
  %t = sea.select %c1, %x, %y : i8
  // numeral, ite of numerals (2 conditions), ite with a symbolic leaf, two
  // non-constant terms
  %a = sea.mul %y, %k3 : i8 loc("k.c":2:3)
  %b = sea.mul %y, %s : i8 loc("k.c":3:3)
  %c = sea.mul %y, %t : i8 loc("k.c":4:3)
  %d = sea.mul %x, %y : i8 loc("k.c":5:3)
  %e = sea.udiv %x, %y : i8 loc("k.c":6:3)
  %s1 = sea.add %a, %b : i8
  %s2 = sea.add %s1, %c : i8
  %s3 = sea.add %s2, %d : i8
  %s4 = sea.add %s3, %e : i8
  %k17 = sea.const 17 : i8
  %ok = sea.icmp ne, %s4, %k17 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
