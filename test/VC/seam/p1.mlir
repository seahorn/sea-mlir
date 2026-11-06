// P1 of the SeaM thesis (Fig. 3.1): two reads over writes, one through a
// pointer chosen by a nondet condition.
//   num = malloc(4) (obj8); *num = 4; st = malloc(8) (obj16);
//   st->a = 21, st->b = 42; p = nd ? &st->a : &st->b; *p = 4;
//   assert(*num == st->b)
// Fields are i32 cells of a word memory (offsets 0 and 4 here). The unsimplified
// reads (Fig. 3.3a, 3.4a) are ite chains over all five writes; SeaM resolves
// them to Fig. 3.3b (the first read is the constant 4) and Fig. 3.4b
// (the second is ite(!a, 4, 42)). Check: no select and no store survive, and
// the stats show the rules that did it.
// RUN: sea-vc %s --lower --widen-memory=4 --seam --seam-stats | FileCheck %s --check-prefix=STATS
// RUN: sea-vc %s --lower --widen-memory=4 --seam --seam-simplify=0 --smt2 | FileCheck %s --check-prefix=SMT
// RUN: sea-vc %s --lower --widen-memory=4 --seam --smt2 | FileCheck %s --check-prefix=SIMP
// RUN: sea-vc %s --lower --widen-memory=4 --smt2 | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --widen-memory=4 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --widen-memory=4 --seam | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --widen-memory=4 --seam --seam-inb=0 | FileCheck %s --check-prefix=VERDICT
// SMT-NOT: select
// SMT-NOT: store
// PLAIN: select
// VERDICT: result: {{(sat|unsat)}}
// STATS-DAG: seam.rule.row.store-hit:
// STATS-DAG: seam.rule.row.store-skip:
// STATS-DAG: seam.rule.cmp.diff-base:
// The second read, as the thesis simplifies it (Fig. 3.4b; names are the VC's):
// SMT: (ite (not (= nd.{{.*}} #b1)) #x00000004 #x0000002a)
// With the normalization rules (the default under --seam) the compare of
// 4 with that ite is the condition itself, and the whole query is `nd = 1`.
// SIMP-NOT: ite
// SIMP: (= nd.{{.*}} #b1)
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @p1 {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %c8 = sea.const 8 : i64
  %num = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %v4 = sea.const 4 : i32
  %m1 = sea.store %m0[%num], %v4 inplace : !m, i32
  %st = sea.malloc %c8 align 4 [2] : i64 -> !sea.ptr<64>
  %v21 = sea.const 21 : i32
  %m2 = sea.store %m1[%st], %v21 inplace : !m, i32
  %stb = sea.ptr.add %st, %c4 : !sea.ptr<64>, i64
  %v42 = sea.const 42 : i32
  %m3 = sea.store %m2[%stb], %v42 inplace : !m, i32
  %nd = sea.nondet "nd_bool" [3] : i1
  %p = sea.select %nd, %st, %stb : !sea.ptr<64>
  %m4 = sea.store %m3[%p], %v4 inplace : !m, i32
  %x = sea.load %m4[%num] : !m -> i32
  %y = sea.load %m4[%stb] : !m -> i32
  %eq = sea.icmp eq, %x, %y : i32
  sea.assert %eq
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
