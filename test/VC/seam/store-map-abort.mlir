// R-ABORT (thesis Fig. 6.5): a read at a symbolic offset of an object whose
// writes are a store-map. Five byte writes to the object, then a read at
// base + i with i in [0, 4] (R-ABORT: the map is read as the chain of its
// entries, compares i = k, and ends in the memory below). The assertion `the
// byte is one of the five values` holds (unsat); the second program asserts
// that it is not 42, which fails for i = 1 (sat: the model is replayed, so
// the answer is checked against the interpreter with store-maps in the VC).
// Same answers with Store-Map off, without A-INB, and in the plain VC.
// RUN: sea-vc %s --lower --program holds | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program holds --seam --seam-stats | FileCheck %s --check-prefix=STATS --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program holds --seam --seam-store-map=0 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program holds --seam --seam-inb=0 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program holds --seam --seam-row-expand=0 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program holds --seam --smt2 | FileCheck %s --check-prefix=SMT
// RUN: sea-vc %s --lower --program fails | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program fails --seam | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program fails --seam --seam-store-map=0 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program fails --seam --seam-inb=0 | FileCheck %s --check-prefix=SAT
// STATS-DAG: seam.rule.smap.new: 1
// STATS-DAG: seam.rule.row.smap-abort: {{[1-9]}}
// STATS-DAG: seam.rule.smap.expanded: 0
// UNSAT: result: unsat
// SAT: result: sat
// SAT: replay: assert-failed
// SAT: validate: checked={{[0-9]+}} mismatches=0
// No select over a store-map, no store-map, in the query
// SMT-NOT: smap
// SMT-NOT: store-map
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @holds {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %o = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c3 = sea.const 3 : i64
  %c4 = sea.const 4 : i64
  %p1 = sea.ptr.add %o, %c1 : !sea.ptr<64>, i64
  %p2 = sea.ptr.add %o, %c2 : !sea.ptr<64>, i64
  %p3 = sea.ptr.add %o, %c3 : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %o, %c4 : !sea.ptr<64>, i64
  %v21 = sea.const 21 : i8
  %v42 = sea.const 42 : i8
  %v7 = sea.const 7 : i8
  %v9 = sea.const 9 : i8
  %v5 = sea.const 5 : i8
  %m1 = sea.store %m0[%o], %v21 inplace : !m, i8
  %m2 = sea.store %m1[%p1], %v42 inplace : !m, i8
  %m3 = sea.store %m2[%p2], %v7 inplace : !m, i8
  %m4 = sea.store %m3[%p3], %v9 inplace : !m, i8
  %m5 = sea.store %m4[%p4], %v5 inplace : !m, i8
  %i = sea.nondet "i" [2] : i64
  %le = sea.icmp ule, %i, %c4 : i64
  sea.assume %le
  %p = sea.ptr.add %o, %i : !sea.ptr<64>, i64
  %y = sea.load %m5[%p] : !m -> i8
  %e1 = sea.icmp eq, %y, %v21 : i8
  %e2 = sea.icmp eq, %y, %v42 : i8
  %e3 = sea.icmp eq, %y, %v7 : i8
  %e4 = sea.icmp eq, %y, %v9 : i8
  %e5 = sea.icmp eq, %y, %v5 : i8
  %o1 = sea.or %e1, %e2 : i1
  %o2 = sea.or %o1, %e3 : i1
  %o3 = sea.or %o2, %e4 : i1
  %ok = sea.or %o3, %e5 : i1
  sea.assert %ok
  sea.halt
}
sea.program @fails {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %o = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c4 = sea.const 4 : i64
  %p1 = sea.ptr.add %o, %c1 : !sea.ptr<64>, i64
  %p2 = sea.ptr.add %o, %c2 : !sea.ptr<64>, i64
  %v21 = sea.const 21 : i8
  %v42 = sea.const 42 : i8
  %v7 = sea.const 7 : i8
  %m1 = sea.store %m0[%o], %v21 inplace : !m, i8
  %m2 = sea.store %m1[%p1], %v42 inplace : !m, i8
  %m3 = sea.store %m2[%p2], %v7 inplace : !m, i8
  %i = sea.nondet "i" [2] : i64
  %le = sea.icmp ule, %i, %c4 : i64
  sea.assume %le
  %p = sea.ptr.add %o, %i : !sea.ptr<64>, i64
  %y = sea.load %m3[%p] : !m -> i8
  %ne = sea.icmp ne, %y, %v42 : i8
  sea.assert %ne
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
