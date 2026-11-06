// SeaM at a join: the memory merged by the ite join encoding is an ite, and a
// read after the join is pushed through it (row.ite) down to the stores of
// each branch, where the object-aware compare decides each cell. Two objects,
// each branch writes a different one; the read of the object the branch did not
// write sees the value written before the branch.
// RUN: sea-vc %s --lower --seam --seam-stats | FileCheck %s --check-prefix=STATS
// RUN: sea-vc %s --lower --seam --seam-simplify=0 --smt2 | FileCheck %s --check-prefix=SMT
// RUN: sea-vc %s --lower --seam --smt2 | FileCheck %s --check-prefix=SMT
// RUN: sea-vc %s --lower --seam | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VERDICT
// RUN: not sea-vc %s --lower --seam-stats 2>&1 | FileCheck %s --check-prefix=NOSEAM
// RUN: sea-vc %s --lower --seam --seam-inb=0 | FileCheck %s --check-prefix=VERDICT
// VERDICT: result: unsat
// STATS-DAG: seam.rule.row.ite:
// STATS-DAG: seam.rule.row.store-hit:
// STATS-DAG: seam.rule.row.store-skip:
// SMT-NOT: select
// SMT-NOT: store
// NOSEAM: --seam or --simplify
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @join {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c8 = sea.const 8 : i64
  %o1 = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %o2 = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %v1 = sea.const 1 : i8
  %v2 = sea.const 2 : i8
  %v3 = sea.const 3 : i8
  %c = sea.nondet "nd" [3] : i1
  %ma = sea.store %m0[%o1], %v1 inplace : !m, i8
  %mb = sea.store %ma[%o2], %v2 inplace : !m, i8
  sea.cond_br %c, ^t, ^e
^t:
  %mt = sea.store %mb[%o1], %v3 inplace : !m, i8
  sea.br ^j(%mt : !m)
^e:
  %me = sea.store %mb[%o2], %v3 inplace : !m, i8
  sea.br ^j(%me : !m)
^j(%mj : !m):
  %x = sea.load %mj[%o1] : !m -> i8
  %y = sea.load %mj[%o2] : !m -> i8
  %s = sea.icmp ne, %x, %y : i8
  sea.assert %s
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
