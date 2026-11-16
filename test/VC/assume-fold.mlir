// RUN: sea-vc %s --program=fold --lower | FileCheck %s --check-prefix=ON
// RUN: sea-vc %s --program=fold --lower --lower-disable=assume-fold | FileCheck %s --check-prefix=OFF
// RUN: sea-vc %s --program=fold --lower --vcgen-only --print-lowered=%t.low.mlir
// RUN: sea-run %t.low.mlir --program=fold --input=0=1,1=5 | FileCheck %s --check-prefix=RUN-FAIL
// RUN: sea-run %s --program=fold --input=0=1,1=5 | FileCheck %s --check-prefix=RUN-FAIL
// RUN: sea-run %t.low.mlir --program=fold --input=0=0,1=5 | FileCheck %s --check-prefix=RUN-BLOCKED
// RUN: sea-run %s --program=fold --input=0=0,1=5 | FileCheck %s --check-prefix=RUN-BLOCKED
// RUN: sea-run %t.low.mlir --program=fold --input=0=1,1=4 | FileCheck %s --check-prefix=RUN-HALT
// RUN: sea-run %s --program=fold --input=0=1,1=4 | FileCheck %s --check-prefix=RUN-HALT
// RUN: sea-vc %s --program=sibling --lower | FileCheck %s --check-prefix=SIBLING
// RUN: sea-vc %s --program=sibling --lower --seam --split-candidates | FileCheck %s --check-prefix=CAND
// RUN: sea-vc %s --program=held_negated --lower --replay-original 2>&1 | FileCheck %s --check-prefix=NEG
// RUN: sea-vc %s --program=held_false --lower --lower-disable=assume-fold --replay-original 2>&1 | FileCheck %s --check-prefix=HFALSE
// RUN: sea-vc %s --program=asserts_sib --lower --replay-original 2>&1 | FileCheck %s --check-prefix=ASIB
// RUN: sea-vc %s --program=two_asserts --lower 2>&1 | FileCheck %s --check-prefix=TWO
// RUN: sea-vc %s --program=edge --lower --replay-original 2>&1 | FileCheck %s --check-prefix=EDGE
// RUN: sea-vc %s --program=edge --lower --lower-disable=assume-fold 2>&1 | FileCheck %s --check-prefix=EDGE-OFF
// RUN: sea-vc %s --program=edge_join --lower --replay-original 2>&1 | FileCheck %s --check-prefix=EJOIN
// RUN: sea-vc %s --program=int_eq --lower --seam --vcgen-only --dag-stats 2>&1 | FileCheck %s --check-prefix=INTEQ
// RUN: sea-vc %s --program=int_eq_sat --lower --replay-original 2>&1 | FileCheck %s --check-prefix=INTSAT
// RUN: sea-vc %s --program=int_eq_chain_edge --lower --replay-original 2>&1 | FileCheck %s --check-prefix=CHAIN
// RUN: sea-vc %s --program=select_and --lower --range-fold=on 2>&1 | FileCheck %s --check-prefix=SAND
// sea-assume-fold in the lowering (docs/design/AssumeFold.md). @fold: the
// branch on the assumed %c folds and its dead arm goes; the answer and the
// replay are the same either way, and the lowered program runs as the
// original on the same inputs. With the fold off, sea-vc reports the
// branch it leaves (vc.assumed-branches).
// ON: vc.assume-fold: uses=1 by-assume=1 by-assert=0 by-edge=0 values=1 branches=1 selects=0 asserts-held=0 asserts-failed=0 blocks=1
// ON-NOT: vc.assumed-branches
// ON: result: sat
// ON: replay: assert-failed
// ON: validate: checked={{[0-9]+}} mismatches=0
// OFF-NOT: vc.assume-fold
// OFF: vc.assumed-branches: branches=1 selects=0 values=1
// OFF: result: sat
// OFF: replay: assert-failed
// OFF: validate: checked={{[0-9]+}} mismatches=0
// RUN-FAIL: status: assert-failed
// RUN-BLOCKED: status: assume-blocked
// RUN-HALT: status: halted
// @sibling: one arm assumes %c; the select after the join is not under
// the assume: not folded (a fold there makes the job unsat), reported, and
// listed as a split candidate.
// SIBLING-NOT: vc.assume-fold
// SIBLING: vc.assumed-branches: branches=0 selects=1 values=1
// SIBLING: result: sat
// SIBLING: replay: assert-failed
// CAND: split.assumed-candidate: name=nd.0 assumed=1 branches=0 selects=1
// @held_negated: `assume c; assert c; assert !c`: the fold makes the first
// assert 1 (dropped) and the second 0 (a direct failure); the replay names
// the second, as the original program fails it.
// NEG: vc.assume-fold: {{.*}}asserts-held=1 asserts-failed=1
// NEG: replay: assert-op: x.c:4:3
// NEG: replay.original: status=assert-failed at=x.c:4:3 match=yes
// @held_false, fold off: `assume c; assert c; assert 0`: the fold-off path
// names the failing assert too.
// HFALSE: replay: assert-op: x.c:9:3
// HFALSE: replay.original: status=assert-failed at=x.c:9:3 match=yes
sea.program @fold {
  %t = sea.const 1 : i1
  %k = sea.const 5 : i8
  %c = sea.nondet "c" [0] : i1
  %x = sea.nondet "x" [1] : i8
  sea.assume %c
  sea.cond_br %c, ^bb1, ^bb2
^bb1:
  %ok = sea.icmp ne, %x, %k : i8
  sea.br ^bb3(%ok : i1)
^bb2:
  sea.br ^bb3(%t : i1)
^bb3(%v: i1):
  sea.assert %v
  sea.halt
}

sea.program @sibling {
  %k = sea.const 5 : i8
  %c = sea.nondet "c" [0] : i1
  %x = sea.nondet "x" [1] : i8
  %y = sea.nondet "y" [2] : i8
  %p = sea.nondet "p" [3] : i1
  sea.cond_br %p, ^bb1, ^bb2
^bb1:
  sea.assume %c
  sea.br ^bb3
^bb2:
  sea.br ^bb3
^bb3:
  %s = sea.select %c, %x, %y : i8
  %ok = sea.icmp ne, %s, %k : i8
  sea.assert %ok
  sea.halt
}

sea.program @held_negated {
  %t = sea.const 1 : i1
  %c = sea.nondet "c" [0] : i1
  sea.assume %c
  sea.assert %c loc("x.c":3:3)
  %nc = sea.xor %c, %t : i1
  sea.assert %nc loc("x.c":4:3)
  sea.halt
}

// @asserts_sib: an assert in one arm is no fact at the join: the second
// assert still fails (sat) and the replay names it.
// ASIB-NOT: vc.assume-fold
// ASIB: result: sat
// ASIB: replay.original: {{.*}}match=yes
// @two_asserts: nothing to fold; the assumes sea-merge-asserts inserts are
// not reported as assumed branches.
// TWO-NOT: vc.assumed-branches
// TWO: result: sat
sea.program @asserts_sib {
  %c = sea.nondet "c" [0] : i1
  %p = sea.nondet "p" [1] : i1
  sea.cond_br %p, ^a, ^b
^a:
  sea.assert %c loc("x.c":1:1)
  sea.br ^j
^b:
  sea.br ^j
^j:
  sea.assert %c loc("x.c":2:1)
  sea.halt
}

sea.program @two_asserts {
  %k = sea.const 5 : i8
  %x = sea.nondet "x" [0] : i8
  %y = sea.nondet "y" [1] : i8
  %a = sea.icmp ne, %x, %k : i8
  %b = sea.icmp ne, %y, %k : i8
  sea.assert %a loc("x.c":1:1)
  sea.cond_br %b, ^t, ^e
^t:
  sea.br ^j
^e:
  sea.br ^j
^j:
  sea.assert %b loc("x.c":2:1)
  sea.halt
}

sea.program @held_false {
  %c = sea.nondet "c" [0] : i1
  sea.assume %c loc("h.c":2:3)
  sea.assert %c loc("x.c":3:3)
  %f = sea.const 0 : i1
  sea.assert %f loc("x.c":9:3)
  sea.halt
}


// @edge: facts of cond_br edges. Under the true edge of `cond_br %c` a
// second branch on %c folds, under the false edge a select on %c: one
// block goes (the branch's false arm). sat either way, the replay matches
// the original program.
// EDGE: vc.assume-fold: uses=2 by-assume=0 by-assert=0 by-edge=2 values=1 branches=1 selects=1 asserts-held=0 asserts-failed=0 blocks=1
// EDGE: result: sat
// EDGE: replay.original: {{.*}}match=yes
// EDGE-OFF-NOT: vc.assume-fold
// EDGE-OFF: result: sat
sea.program @edge {
  %c5 = sea.const 5 : i8
  %x = sea.nondet "x" [0] : i8
  %y = sea.nondet "y" [1] : i8
  %c = sea.icmp ult, %x, %c5 : i8
  sea.cond_br %c, ^a, ^b
^a:
  sea.cond_br %c, ^a1, ^a2
^a1:
  sea.br ^j(%x : i8)
^a2:
  sea.br ^j(%y : i8)
^b:
  %s = sea.select %c, %x, %y : i8
  sea.br ^j(%s : i8)
^j(%r : i8):
  %e = sea.icmp ne, %r, %c5 : i8
  sea.assert %e loc("x.c":7:1)
  sea.halt
}

// @edge_join: ^j is entered on the true edge of `cond_br %c` (a critical
// edge here: the lowering splits it last) and from ^b, where %c is 0. %c
// is not fixed in ^j: the assert of %c stays and fails (sat; folding it by
// the true edge alone would make the job unsat).
// EJOIN-NOT: vc.assume-fold
// EJOIN: result: sat
// EJOIN: replay.original: {{.*}}match=yes
sea.program @edge_join {
  %c = sea.nondet "c" [0] : i1
  %p = sea.nondet "p" [1] : i1
  sea.cond_br %c, ^j, ^b
^b:
  sea.cond_br %p, ^j, ^z
^z:
  sea.br ^j
^j:
  sea.assert %c loc("x.c":9:1)
  sea.halt
}

// @select_and: LLVM's short-circuit form of `0 <= x && x < 6`, `select
// %a, %b, false`, assumed: a conjunction (matchConnective), so
// sea-range-fold takes both bounds as facts and settles `x ult 9` (read as
// one opaque conjunct it has no fact). unsat either way.
// SAND: vc.range-fold: {{.*}} facts=2 facts-skipped=0 facts-used={{[1-9][0-9]*}} icmp=1
// SAND: result: unsat
sea.program @select_and {
  %c0 = sea.const 0 : i8
  %c6 = sea.const 6 : i8
  %c9 = sea.const 9 : i8
  %f = sea.const 0 : i1
  %x = sea.nondet "x" [0] : i8
  %y = sea.nondet "y" [1] : i8
  %a = sea.icmp sge, %x, %c0 : i8
  %b = sea.icmp slt, %x, %c6 : i8
  %ab = sea.select %a, %b, %f : i1
  sea.assume %ab
  %lt = sea.icmp ult, %y, %c6 : i8
  %ok1 = sea.icmp ult, %x, %c9 : i8
  %ok = sea.or %ok1, %lt : i1
  sea.assert %ok
  sea.halt
}


// @int_eq: `assume (x == 5)`, then x scales an address: with x replaced
// by 5 the store's address is a constant and the VC is smaller (the
// reduced shape of firedancer's step jobs).
// INTEQ: vc.dag-size: {{([0-9]|[0-9][0-9]|10[0-9])$}}
// INTEQ: vc.assume-fold: {{.*}}int-uses=2 int-values=1
sea.program @int_eq {
  %c4 = sea.const 4 : i64
  %c5 = sea.const 5 : i64
  %k7 = sea.const 7 : i32
  %m0 = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
  %b = sea.segment "stack.1" base 3221225456 : i64 size 64 : i64 align 8 kind "stack" : !sea.addr<64>
  %x = sea.nondet "x" [1] : i64
  %y = sea.nondet "y" [2] : i64
  %e = sea.icmp eq, %x, %c5 : i64
  sea.assume %e
  %yb = sea.icmp ult, %y, %c5 : i64
  sea.assume %yb
  %ox = sea.mul %x, %c4 : i64
  %oy = sea.mul %y, %c4 : i64
  %px = sea.addr.add %b, %ox : !sea.addr<64>, i64
  %py = sea.addr.add %b, %oy : !sea.addr<64>, i64
  %m1 = sea.store %m0[%px], %k7 inplace : !sea.mem<!sea.addr<64>, i8>, i32
  %v0 = sea.load %m0[%py] : !sea.mem<!sea.addr<64>, i8> -> i32
  %v1 = sea.load %m1[%py] : !sea.mem<!sea.addr<64>, i8> -> i32
  %lt = sea.icmp ult, %y, %x : i64
  sea.cond_br %lt, ^a, ^b
^a:
  %g = sea.icmp eq, %v0, %v1 : i32
  sea.assert %g
  sea.halt
^b:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// @int_eq_sat: on the true edge of `cond_br (x == 7)` x is 7; the assert
// there fails, and the model of x replays on the original program.
// INTSAT: vc.assume-fold: {{.*}}int-uses=1 int-values=1
// INTSAT: result: sat
// INTSAT: replay.original: {{.*}}match=yes
sea.program @int_eq_sat {
  %c1 = sea.const 1 : i8
  %c7 = sea.const 7 : i8
  %c8 = sea.const 8 : i8
  %x = sea.nondet "x" [0] : i8
  %e = sea.icmp eq, %x, %c7 : i8
  sea.cond_br %e, ^a, ^b
^a:
  %s = sea.add %x, %c1 : i8
  %ok = sea.icmp ne, %s, %c8 : i8
  sea.assert %ok loc("x.c":11:1)
  sea.halt
^b:
  sea.halt
}


// @int_eq_chain_edge: x == 5 makes `icmp eq y, x` the equality y == 5 on
// the true edge (a second round); the assert there fails (sat) and the
// model replays on the original program.
// CHAIN: vc.assume-fold: {{.*}}int-uses=2 int-values=2 rounds=2
// CHAIN: result: sat
// CHAIN: replay.original: {{.*}}match=yes
sea.program @int_eq_chain_edge {
  %c1 = sea.const 1 : i64
  %c5 = sea.const 5 : i64
  %c6 = sea.const 6 : i64
  %x = sea.nondet "x" [0] : i64
  %y = sea.nondet "y" [1] : i64
  %e = sea.icmp eq, %x, %c5 : i64
  sea.assume %e
  %e2 = sea.icmp eq, %y, %x : i64
  sea.cond_br %e2, ^a, ^b
^a:
  %z = sea.add %y, %c1 : i64
  %ok = sea.icmp ne, %z, %c6 : i64
  sea.assert %ok loc("x.c":21:1)
  sea.halt
^b:
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
