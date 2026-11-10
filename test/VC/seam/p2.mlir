// P2 of the SeaM thesis (Fig. 3.5): six writes to three cells, then reads.
//   num = malloc(4) (obj8); st = malloc(8) (obj16);
//   st->a = 21; *num = 4; st->b = 42; st->a = 42; *num = 8; st->b = 21;
//   assert(*num < st->b && st->a == 42)
// Fig. 3.6a / 3.6b: only the last write to each cell matters. Each read is a
// chain of up to six stores of which SeaM skips the cells of other objects and
// offsets and hits the last write to its own: the VC has no select and no
// store, and the assertion holds (unsat). Without SeaM the reads stay selects.
// Read-over-write rules (Store-Map off: store-map.mlir has the same program with it on):
// The rule counts are of the three reads as written: --lower-opt=false, else
// canonicalize forwards the last store's value to the read of st->b.
// RUN: sea-vc %s --lower --lower-opt=false --widen-memory=4 --seam --seam-store-map=0 --seam-stats | FileCheck %s --check-prefix=STATS
// RUN: sea-vc %s --lower --widen-memory=4 --seam --smt2 | FileCheck %s --check-prefix=SMT
// RUN: sea-vc %s --lower --widen-memory=4 --smt2 | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --widen-memory=4 --seam | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --widen-memory=4 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --widen-memory=4 --seam --seam-inb=0 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --widen-memory=4 --seam --seam-store-map=0 --seam-inb=0 --seam-stats | FileCheck %s --check-prefix=NOINB
// VERDICT: result: unsat
// SMT-NOT: select
// SMT-NOT: store
// PLAIN: select
// STATS-DAG: seam.rule.row.store-hit: 3
// STATS-DAG: seam.rule.row.store-skip:
// Without A-INB nothing is assumed: no compare of different objects is decided,
// the verdict is the same.
// NOINB-NOT: seam.rule.cmp.diff-base
// NOINB: seam.rule.row.store-hit:
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @p2 {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %c8 = sea.const 8 : i64
  %num = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %st = sea.malloc %c8 align 4 [2] : i64 -> !sea.ptr<64>
  %stb = sea.ptr.add %st, %c4 : !sea.ptr<64>, i64
  %v21 = sea.const 21 : i32
  %v4 = sea.const 4 : i32
  %v42 = sea.const 42 : i32
  %v8 = sea.const 8 : i32
  %m1 = sea.store %m0[%st], %v21 inplace : !m, i32
  %m2 = sea.store %m1[%num], %v4 inplace : !m, i32
  %m3 = sea.store %m2[%stb], %v42 inplace : !m, i32
  %m4 = sea.store %m3[%st], %v42 inplace : !m, i32
  %m5 = sea.store %m4[%num], %v8 inplace : !m, i32
  %m6 = sea.store %m5[%stb], %v21 inplace : !m, i32
  %x = sea.load %m6[%num] : !m -> i32
  %y = sea.load %m6[%stb] : !m -> i32
  %a = sea.load %m6[%st] : !m -> i32
  %lt = sea.icmp ult, %x, %y : i32
  %ea = sea.icmp eq, %a, %v42 : i32
  %ok = sea.and %lt, %ea : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
