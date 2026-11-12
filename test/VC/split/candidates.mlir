// sea-vc --split-candidates (docs/design/Split.md): the pick run of
// --split-auto (encoding, SeaM, the census of the open alias questions; no
// solver), listed, then `result: none`. Every ite-valued address is a
// split.leaves-candidate (for --split-leaves); the i1 values and block
// guards in the ite conditions behind an open question are
// split.bool-candidate lines (for --split).
//
// @alias: %q = select(c, A, p), p loaded from nondet memory; the store at
// %q leaves the question q ~ A open. SeaM splits it into leaf pairs, so the
// question holds the unknown leaf p, not %q's term (direct=0); no other
// candidate is behind it (own=1); c (nd.5) is the ite condition.
// RUN: sea-vc %s --lower --program=alias --seam --split-candidates | FileCheck %s --check-prefix=ALIAS
// ALIAS: split.candidates: open-aliases=1 leaves-candidates=1 bool-candidates=1 bool-listed=1 bool-dropped=0
// ALIAS: split.leaves-candidate: name=%q leaves=2 unknown-leaves=1 open-aliases=1 direct=0 own=1 loc=src/sel.cc:768:3
// ALIAS: split.bool-candidate: name=nd.5 target=value open-aliases=1 addrs=1
// ALIAS: result: none
// ALIAS-NOT: split.part

// @shared: %q and %s share the unknown leaf p, so the open question counts
// for both (the tie of --split-auto's ranking); only %q's term is in the
// question (the store's address is read at %q): direct tells them apart,
// own=0 says that the attribution overlaps.
// RUN: sea-vc %s --lower --program=shared --seam --split-candidates | FileCheck %s --check-prefix=SHARED
// SHARED: split.leaves-candidate: name=%q leaves=2 unknown-leaves=1 open-aliases=1 direct=1 own=0 loc=src/q.cc:10:3
// SHARED: split.leaves-candidate: name=%s leaves=2 unknown-leaves=1 open-aliases=1 direct=0 own=0 loc=src/s.cc:20:3

// @join: the ite comes from a join: its conditions are the guards of the
// two blocks before it; one question, two guards. --split-candidates-max
// caps the Boolean list and says how many it dropped.
// RUN: sea-vc %s --lower --program=join --seam --split-candidates | FileCheck %s --check-prefix=JOIN
// RUN: sea-vc %s --lower --program=join --seam --split-candidates --split-candidates-max=1 | FileCheck %s --check-prefix=CAP
// JOIN: split.candidates: open-aliases=1 leaves-candidates=1 bool-candidates=2 bool-listed=2 bool-dropped=0
// JOIN: split.leaves-candidate: name=%{{[0-9a-z_.]+}} leaves=2 {{.*}} open-aliases=1
// JOIN: split.bool-candidate: name=^bb{{[0-9]+}} target=guard open-aliases=1 addrs=1
// JOIN: split.bool-candidate: name=^bb{{[0-9]+}} target=guard open-aliases=1 addrs=1
// JOIN: result: none
// CAP: bool-candidates=2 bool-listed=1 bool-dropped=1
// CAP-COUNT-1: split.bool-candidate:
// CAP-NOT: split.bool-candidate:

// --format=jsonl: the records render to the text lines.
// RUN: sea-vc %s --lower --program=alias --seam --split-candidates > %t.txt
// RUN: sea-vc %s --lower --program=alias --seam --split-candidates --format=jsonl > %t.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl \
// RUN:   --require split-candidates:open_aliases,leaves_candidates,bool_candidates,bool_listed,bool_dropped,bool_values,vc_time \
// RUN:   --require split-leaves-candidate:name,loc,leaves,unknown_leaves,open_aliases,direct,own \
// RUN:   --require split-bool-candidate:name,loc,target,open_aliases,addrs \
// RUN:   --require result:value

// It lists, it does not split: not with a split, and it needs --seam;
// options of a split need one.
// RUN: not sea-vc %s --lower --program=alias --seam --split-candidates --split-auto 2>&1 | FileCheck %s --check-prefix=EXCL
// RUN: not sea-vc %s --lower --program=alias --seam --split-candidates --split=nd.5 2>&1 | FileCheck %s --check-prefix=EXCL
// RUN: not sea-vc %s --lower --program=alias --split-candidates 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: not sea-vc %s --lower --program=alias --seam --split-candidates --smt2 2>&1 | FileCheck %s --check-prefix=SMT2
// RUN: not sea-vc %s --lower --program=alias --seam --split-candidates --split-max-parts=2 2>&1 | FileCheck %s --check-prefix=PARTS
// RUN: not sea-vc %s --lower --program=alias --seam --split-candidates-max=2 2>&1 | FileCheck %s --check-prefix=MAX
// EXCL: not with --split, --split-leaves or --split-auto
// SEAM: needs --seam
// SMT2: not with --smt2
// PARTS: splits nothing
// MAX: --split-candidates-max needs --split-candidates

// An ite tree with more paths than --split-max-paths has no leaf cases:
// listed with leaves=0, and a warning. Its question held only the unknown
// leaf, so without leaves nothing is attributed to it.
// RUN: sea-vc %s --lower --program=alias --seam --split-candidates --split-max-paths=1 2>&1 | FileCheck %s --check-prefix=PATHS
// PATHS-DAG: [vc.split-max-paths]
// PATHS-DAG: split.leaves-candidate: name=%q leaves=0 unknown-leaves=0 open-aliases=0

// Nothing is solved, so the checks that need a solve are refused;
// --brunch-stats gives the result as --vcgen-only does.
// RUN: not sea-vc %s --lower --program=alias --seam --split-candidates --seam-check 2>&1 | FileCheck %s --check-prefix=SOLVES
// RUN: not sea-vc %s --lower --program=alias --seam --split-candidates --eq-audit 2>&1 | FileCheck %s --check-prefix=SOLVES
// RUN: not sea-vc %s --lower --program=alias --seam --split-candidates --x-read-table-check=1 2>&1 | FileCheck %s --check-prefix=SOLVES
// RUN: sea-vc %s --lower --program=alias --seam --split-candidates --brunch-stats | FileCheck %s --check-prefix=BRUNCH
// SOLVES: --split-candidates solves nothing
// BRUNCH: BRUNCH_STAT result none

// An unknown verdict of a --seam run (here: a z3 rlimit of 1) whose
// normalized query has open alias questions points to --split-candidates.
// No hint without open questions (@factor), without --seam, or in a split
// run (its parts name the hard case).
// RUN: not sea-vc %s --lower --program=alias --seam --z3-param=rlimit=1 2>/dev/null | FileCheck %s --check-prefix=HINT
// RUN: not sea-vc %s --lower --program=alias --seam --z3-param=rlimit=1 --format=jsonl 2>/dev/null | FileCheck %s --check-prefix=HINTJ
// RUN: not sea-vc %s --program=factor --seam --z3-param=rlimit=1 2>/dev/null | FileCheck %s --check-prefix=NOHINT
// RUN: not sea-vc %s --lower --program=alias --z3-param=rlimit=1 2>/dev/null | FileCheck %s --check-prefix=NOHINT
// RUN: not sea-vc %s --lower --program=alias --seam --z3-param=rlimit=1 --split=nd.5 --split-check=false 2>/dev/null | FileCheck %s --check-prefix=NOHINTSPLIT
// HINT: result: unknown
// HINT: hint: split: open-aliases=1; {{.*}}--split-candidates
// HINTJ: "kind":"hint","topic":"split","open_aliases":1,"option":"--split-candidates"
// NOHINT: result: unknown
// NOHINT-NOT: hint:
// NOHINTSPLIT: split.part-result: unknown
// NOHINTSPLIT-NOT: hint:
// NOHINTSPLIT: result: unknown
// NOHINTSPLIT-NOT: hint:

!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @alias {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c8 align 8 [3] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %c = sea.nondet "c" [5] : i1
  %p = sea.load %m0[%C] : !m -> !sea.ptr<64>
  %q = sea.select %c, %A, %p : !sea.ptr<64> loc("q"("src/sel.cc":768:3))
  %y = sea.load %m0[%A] : !m -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%q], %v inplace : !m, i8
  %x = sea.load %m1[%A] : !m -> i8
  %e1 = sea.icmp eq, %x, %v : i8
  %e2 = sea.icmp eq, %x, %y : i8
  %ok = sea.or %e1, %e2 : i1
  sea.assert %ok
  sea.halt
}
sea.program @shared {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c8 align 8 [3] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %c = sea.nondet "c" [5] : i1
  %d = sea.nondet "d" [6] : i1
  %p = sea.load %m0[%C] : !m -> !sea.ptr<64>
  %q = sea.select %c, %A, %p : !sea.ptr<64> loc("q"("src/q.cc":10:3))
  %s = sea.select %d, %C, %p : !sea.ptr<64> loc("s"("src/s.cc":20:3))
  %r = sea.load %m0[%q] : !m -> !sea.ptr<64>
  %y = sea.load %m0[%A] : !m -> i8
  %z = sea.load %m0[%s] : !m -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%r], %v inplace : !m, i8
  %x = sea.load %m1[%A] : !m -> i8
  %e1 = sea.icmp eq, %x, %v : i8
  %e2 = sea.icmp eq, %x, %y : i8
  %e3 = sea.icmp eq, %x, %z : i8
  %o1 = sea.or %e1, %e2 : i1
  %ok = sea.or %o1, %e3 : i1
  sea.assert %ok
  sea.halt
}
sea.program @join {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c8 align 8 [3] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %c = sea.nondet "c" [5] : i1
  %p = sea.load %m0[%C] : !m -> !sea.ptr<64>
  sea.cond_br %c, ^a, ^b
^a:
  sea.br ^j(%A : !sea.ptr<64>)
^b:
  sea.br ^j(%p : !sea.ptr<64>)
^j(%q : !sea.ptr<64>):
  %y = sea.load %m0[%A] : !m -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%q], %v inplace : !m, i8
  %x = sea.load %m1[%A] : !m -> i8
  %e1 = sea.icmp eq, %x, %v : i8
  %e2 = sea.icmp eq, %x, %y : i8
  %ok = sea.or %e1, %e2 : i1
  sea.assert %ok
  sea.halt
}
sea.program @factor {
  %x = sea.nondet "nx" [0] : i128
  %y = sea.nondet "ny" [1] : i128
  %c1 = sea.const 1 : i128
  %lim = sea.const 4611686018427387904 : i128
  %a = sea.icmp ugt, %x, %c1 : i128
  %b = sea.icmp ugt, %y, %c1 : i128
  %c = sea.icmp ult, %x, %lim : i128
  %d = sea.icmp ult, %y, %lim : i128
  sea.assume %a
  sea.assume %b
  sea.assume %c
  sea.assume %d
  %p = sea.mul %x, %y : i128
  %n = sea.const 5316911983139663422034469018046464031 : i128
  %ok = sea.icmp ne, %p, %n : i128
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
