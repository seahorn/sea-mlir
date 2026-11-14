// --split-leaves on an integer value whose VC term is an ite tree
// (docs/design/Split.md): one part per distinct leaf, the condition the
// disjunction of its path conditions, the same (E)/(X) checks as for an
// address. The kernel: a column index j merged over three paths (one per
// column), then a product by the coefficient and the variable of column j,
// and the identity a_j x_j + sum_{k != j} a_k x_k = sum_k a_k x_k (unsat).
// The product by a path-merged value is what the solver finds hard
// unsplit; each part fixes j. The selector names j's block argument by its
// position (k.cc:9).
// RUN: sea-vc %s --lower --split-leaves=@k.cc:9 | FileCheck %s --check-prefix=LEAVES
// LEAVES-COUNT-3: split.check: level=0 target={{%[0-9]+}} cases=3 exhaustive=ok exact=ok
// LEAVES: split.verdict: unsat parts=3 unsat=3 sat=0 unknown=0 {{.*}} check-unknown=0 check=on leaf-structural=3
// LEAVES: result: unsat

// --split-check-rlimit bounds each check (z3's rlimit): at 1 every check is
// unknown, warned, counted; the verdict stands.
// RUN: sea-vc %s --lower --split-leaves=@k.cc:9 --split-check-rlimit=1 2>&1 | FileCheck %s --check-prefix=RLIMIT
// RLIMIT: [vc.split-check-unknown]
// RLIMIT: split.check: level=0 target={{%[0-9]+}} cases=3 exhaustive=unknown exact=unknown
// RLIMIT: split.verdict: unsat {{.*}} check-unknown=3

// The checks catch a broken integer construction (planted): exit 5.
// RUN: not sea-vc %s --lower --split-leaves=@k.cc:9 --x-split-plant=drop-case 2>&1 | FileCheck %s --check-prefix=DROP
// RUN: not sea-vc %s --lower --split-leaves=@k.cc:9 --x-split-plant=wrong-value 2>&1 | FileCheck %s --check-prefix=WRONG
// DROP: [vc.split-check-failed]
// DROP: exhaustive=failed
// WRONG: [vc.split-check-failed]
// WRONG: exact=failed

// An i1 value is a --split target, not a --split-leaves one.
// RUN: not sea-vc %s --lower --split-leaves=nd.3 2>&1 | FileCheck %s --check-prefix=KIND
// KIND: not an address or an integer wider than i1 [vc.split-target]

// --split-candidates ranks the integer and Boolean values by the
// arithmetic they control (split.arith-candidate, a heuristic): the merged
// index first.
// RUN: sea-vc %s --lower --seam --split-candidates | FileCheck %s --check-prefix=CAND
// CAND: split.arith-candidates: arith-nodes={{[1-9][0-9]*}} {{.*}} candidates={{[1-9][0-9]*}}
// CAND-NEXT: split.arith-candidate: name={{%[0-9]+}} target=int arith={{[1-9][0-9]*}} {{.*}} cases=3 {{.*}}loc=k.cc:9:3
// CAND: result: none

// --format=jsonl: the new records render to their text lines.
// RUN: sea-vc %s --lower --seam --split-candidates > %t.txt
// RUN: sea-vc %s --lower --seam --split-candidates --format=jsonl > %t.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl \
// RUN:   --require split-arith-candidates:arith_nodes,int_values,candidates,listed,dropped \
// RUN:   --require split-arith-candidate:name,loc,target,arith,mul,div,direct,feeds,cases,same

sea.program @kernel {
  %x0 = sea.nondet "x" [0] : i16
  %x1 = sea.nondet "x" [1] : i16
  %x2 = sea.nondet "x" [2] : i16
  %b0 = sea.nondet "b" [3] : i1
  %b1 = sea.nondet "b" [4] : i1
  %c0 = sea.const 0 : i16
  %c1 = sea.const 1 : i16
  %c2 = sea.const 2 : i16
  sea.cond_br %b0, ^col0, ^more
^col0:
  sea.br ^join(%c0 : i16)
^more:
  sea.cond_br %b1, ^col1, ^col2
^col1:
  sea.br ^join(%c1 : i16)
^col2:
  sea.br ^join(%c2 : i16)
^join(%j : i16 loc("k.cc":9:3)):
  %a0 = sea.const -1 : i16
  %a1 = sea.const -2 : i16
  %a2 = sea.const 3 : i16
  %j0 = sea.icmp eq, %j, %c0 : i16
  %j1 = sea.icmp eq, %j, %c1 : i16
  %a12 = sea.select %j1, %a1, %a2 : i16
  %aj = sea.select %j0, %a0, %a12 : i16 loc("k.cc":10:3)
  %x12 = sea.select %j1, %x1, %x2 : i16
  %xj = sea.select %j0, %x0, %x12 : i16 loc("k.cc":11:3)
  %p = sea.mul %aj, %xj : i16 loc("k.cc":12:3)
  %p0 = sea.mul %a0, %x0 : i16
  %p1 = sea.mul %a1, %x1 : i16
  %p2 = sea.mul %a2, %x2 : i16
  %s01 = sea.add %p0, %p1 : i16
  %all = sea.add %s01, %p2 : i16
  %j2 = sea.icmp eq, %j, %c2 : i16
  %o0 = sea.select %j0, %c0, %p0 : i16
  %o1 = sea.select %j1, %c0, %p1 : i16
  %o2 = sea.select %j2, %c0, %p2 : i16
  %so = sea.add %o0, %o1 : i16
  %so2 = sea.add %so, %o2 : i16
  %lhs = sea.add %p, %so2 : i16
  %ok = sea.icmp eq, %lhs, %all : i16
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
