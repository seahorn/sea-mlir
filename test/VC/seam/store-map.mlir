// Store-Map (thesis Ch. 6) on the thesis' P2 (Fig. 3.5): six writes to
// three cells of two objects. Allocation order: num (obj8) is first, st (obj16)
// second. The writes to st at offsets 0 and 4 become a store-map (SMAP-NEW, then
// SMAP-HIT for the third write to offset 0 and the sixth to offset 4); the
// writes to num go below it (WOW-COMMUTE, WOSMAP-COMMUTE: allocated earlier)
// and the two writes to num collapse. Reads: num is skipped past the store-map
// (R-SKIP) and hits the single write; the two cells of st are R-HITs: the
// distinct addresses are 3, the store-map has two entries and the chain below
// it one. No select and no store reach the solver (Fig. 3.6).
// RUN: sea-vc %s --lower --widen-memory=4 --seam --seam-stats | FileCheck %s --check-prefix=STATS
// RUN: sea-vc %s --lower --widen-memory=4 --seam --smt2 | FileCheck %s --check-prefix=SMT
// RUN: sea-vc %s --lower --widen-memory=4 --seam | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --widen-memory=4 --seam --seam-store-map=0 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --widen-memory=4 --seam --seam-inb=0 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --widen-memory=4 --seam --seam-store-map=0 --seam-stats | FileCheck %s --check-prefix=NOSMAP
// RUN: sea-vc %s --lower --widen-memory=4 --seam --seam-inb=0 --seam-stats | FileCheck %s --check-prefix=NOINB
// RUN: not sea-vc %s --lower --seam-store-map=0 2>&1 | FileCheck %s --check-prefix=NOSEAM
// VERDICT: result: unsat
// STATS-DAG: seam.rule.smap.new: 1
// STATS-DAG: seam.rule.smap.insert: 2
// STATS-DAG: seam.rule.smap.commute: 1
// STATS-DAG: seam.rule.wow.commute: 1
// STATS-DAG: seam.rule.wow.collapse: 1
// STATS-DAG: seam.rule.row.smap-hit: 2
// STATS-DAG: seam.rule.row.smap-skip: 1
// No store-map reaches the solver; the time lines are there
// STATS-DAG: seam.rule.smap.expanded: 0
// STATS-DAG: vc-time: {{[0-9.]+}}
// STATS-DAG: seam.rewrite-time: {{[0-9.]+}}
// STATS-DAG: solve-time: {{[0-9.]+}}
// SMT-NOT: select
// SMT-NOT: store
// SMT-NOT: smap
// Store-Map off: no store-map rule fires
// NOSMAP-NOT: smap
// NOSMAP: seam.rule.row.store-hit: 3
// Without A-INB nothing commutes and nothing is skipped across objects; the
// writes to st still group (same object, numeral offsets: no assumption) and
// R-HIT and R-MISS are decided without one
// NOINB-NOT: seam.rule.wow.commute
// NOINB-NOT: seam.rule.smap.commute
// NOINB-NOT: seam.rule.row.smap-skip
// NOINB: seam.rule.smap.new: {{[1-9]}}
// NOSEAM: need --seam
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @p2sm {
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
