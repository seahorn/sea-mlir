// --ite-stats: the values whose VC terms are ite trees, by kind and source
// location (leaves, conditions, block guards among their atoms), and the
// loads at non-ground addresses by kind; also with --vcgen-only, with and
// without --seam. @merge: a memory and an address merged at a join (two
// leaves each, over the guards), and a load at the merged address.
// RUN: sea-vc %s --lower --program=merge --ite-stats --vcgen-only | FileCheck %s --check-prefix=MERGE
// RUN: sea-vc %s --lower --program=merge --seam --ite-stats --vcgen-only | FileCheck %s --check-prefix=MERGE
// MERGE-DAG: ite.values: kind=mem values=1 over-guards=1
// MERGE-DAG: ite.site: kind=mem values=1 leaves=2 {{.*}} loc=t.c:20:3
// MERGE-DAG: ite.values: kind=addr values=1 over-guards=1
// MERGE-DAG: ite.site: kind=addr values=1 leaves=2 {{.*}} loc=t.c:21:3
// MERGE-DAG: ite.loads: loads=1 dynamic=1 ite=1
// MERGE-DAG: ite.load-site: kind=ite loads=1 loc=t.c:30:3
// MERGE: result: none

// @kernel: an integer index merged over two paths feeds a product (through
// a compare and selects): an int value of two leaves over the guards. %k,
// merged the same way, feeds no arithmetic: not an int value.
// RUN: sea-vc %s --lower --program=kernel --ite-stats --vcgen-only > %t.kernel
// RUN: FileCheck %s --check-prefix=KERNEL < %t.kernel
// RUN: FileCheck %s --check-prefix=NOK < %t.kernel
// KERNEL: ite.site: kind=int values=1 leaves=2 {{.*}} guard-atoms={{[1-9]}} {{.*}}loc=k.cc:9:3
// NOK-NOT: k.cc:40

// --format=jsonl renders the records to their text lines.
// RUN: sea-vc %s --lower --program=merge --ite-stats --vcgen-only > %t.txt
// RUN: sea-vc %s --lower --program=merge --ite-stats --vcgen-only --format=jsonl > %t.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl \
// RUN:   --require ite-values:value_kind,values,over_guards,sites,listed,dropped,leaves_max,capped \
// RUN:   --require ite-site:value_kind,values,leaves,conds,guard_atoms,capped,name,loc \
// RUN:   --require ite-loads:loads,dynamic,ite,ite_index,from_bits,symbolic,sites,listed,dropped \
// RUN:   --require ite-load-site:addr_kind,loads,loc

// Under --smt2 stdout is the query: the ite.* lines go to stderr.
// RUN: sea-vc %s --lower --program=merge --ite-stats --smt2 2> %t.smt2.err > %t.smt2
// RUN: %z3 %t.smt2 | FileCheck %s --check-prefix=Z3
// RUN: FileCheck %s --check-prefix=SMT2ERR < %t.smt2.err
// Z3: {{^}}unsat
// SMT2ERR: ite.values: kind=mem values=1

// One encoding: not with a split; --ite-stats-max needs --ite-stats.
// RUN: not sea-vc %s --lower --program=kernel --ite-stats --split-leaves=@k.cc:9 2>&1 | FileCheck %s --check-prefix=SPLIT
// RUN: not sea-vc %s --lower --program=kernel --ite-stats-max=3 2>&1 | FileCheck %s --check-prefix=MAX
// SPLIT: --ite-stats reports one encoding
// MAX: --ite-stats-max needs --ite-stats

!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @merge {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [3] : !m
  %b = sea.nondet "b" [4] : i1
  %v = sea.const 1 : i8
  sea.cond_br %b, ^l, ^r
^l:
  %m1 = sea.store %m0[%A], %v inplace : !m, i8
  sea.br ^j(%m1, %A : !m, !sea.ptr<64>)
^r:
  %m2 = sea.store %m0[%B], %v inplace : !m, i8
  sea.br ^j(%m2, %B : !m, !sea.ptr<64>)
^j(%m : !m loc("t.c":20:3), %p : !sea.ptr<64> loc("t.c":21:3)):
  %x = sea.load %m[%p] : !m -> i8 loc("t.c":30:3)
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}
sea.program @kernel {
  %x0 = sea.nondet "x" [0] : i16
  %x1 = sea.nondet "x" [1] : i16
  %b0 = sea.nondet "b" [3] : i1
  %c0 = sea.const 0 : i16
  %c1 = sea.const 1 : i16
  sea.cond_br %b0, ^col0, ^col1
^col0:
  sea.br ^join(%c0, %c0 : i16, i16)
^col1:
  sea.br ^join(%c1, %c1 : i16, i16)
^join(%j : i16 loc("k.cc":9:3), %k : i16 loc("k.cc":40:3)):
  %a0 = sea.const -1 : i16
  %a1 = sea.const 3 : i16
  %j0 = sea.icmp eq, %j, %c0 : i16
  %aj = sea.select %j0, %a0, %a1 : i16
  %xj = sea.select %j0, %x0, %x1 : i16
  %p = sea.mul %aj, %xj : i16
  %q = sea.mul %xj, %aj : i16
  %ok1 = sea.icmp eq, %p, %q : i16
  %ok2 = sea.icmp ule, %k, %c1 : i16
  %ok = sea.and %ok1, %ok2 : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
