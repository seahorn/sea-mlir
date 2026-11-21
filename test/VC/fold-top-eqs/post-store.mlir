// RUN: sea-vc %s --program=fold --lower --seam --vc-fold-top-eqs --smt2 2>%t.fold.err | FileCheck %s --check-prefix=FOLD
// RUN: FileCheck %s --check-prefix=FOLD-STATS < %t.fold.err
// RUN: sea-vc %s --program=fold --lower --seam --smt2 2>/dev/null | FileCheck %s --check-prefix=OFF
// RUN: sea-vc %s --program=fold --lower --seam --vc-fold-top-eqs 2>&1 | FileCheck %s --check-prefix=FOLD-UNSAT
// RUN: sea-vc %s --program=trail --lower --seam --vc-fold-top-eqs 2>&1 | FileCheck %s --check-prefix=TRAIL
// RUN: bash -c 'sea-vc %s --program=trail --lower --seam --vc-fold-top-eqs --x-model-trail-plant-skip > %t.skip 2>&1; test $? -eq 2'
// RUN: FileCheck %s --check-prefix=SKIP < %t.skip
// RUN: sea-vc %s --program=trail --lower --seam 2>&1 | FileCheck %s --check-prefix=HEAD
// RUN: not sea-vc %s --program=fold --vc-fold-top-eqs --seam-simplify=0 --lower --seam 2>&1 | FileCheck %s --check-prefix=NOSIMP
// sea-vc --vc-fold-top-eqs (docs/design/FoldTopEqs.md).
//
// @fold: the harness assumes the initial cell p[0] is 7; the code stores to
// p[4] and loads p[0] again. The load reads the post-store memory, so
// sea-assume-fold (initial memory only) does not replace it; after SeaM's
// read-over-write it is the same read as the assumed one, and the fold
// makes it 7: the assertion b * b == 49 folds and the query is false
// (without the fold the product stays: the rewriter does not see b = 7).
// FOLD: (assert false)
// FOLD-STATS: vc.fold-top-eqs: facts={{[1-9][0-9]*}} term={{[1-9][0-9]*}} {{.*}}uses={{[1-9][0-9]*}}
// OFF-NOT: (assert false)
// OFF: (check-sat)
// FOLD-UNSAT: result: unsat
//
// @trail: the same fact, then a store at a symbolic offset of p before the
// load: the load is ite(q = p, 9, 7) after the fold, and the read of the
// assumed cell survives only in its fact, which the second step moves to the
// model trail. The model extended by the trail gives the cell its 7; without
// the trail (testing only: --x-model-trail-plant-skip) the replay reads
// another value, the assume blocks the run, and sea-vc reports the
// disagreement (status 2).
// TRAIL: vc.model-trail: cells=1
// TRAIL: result: sat
// TRAIL: vc.ackermann-model: defaulted={{[0-9]+}} inconsistent=0
// TRAIL: replay: assert-failed
// TRAIL: validate: checked={{[0-9]+}} mismatches=0
// SKIP: result: sat
// SKIP-NOT: replay: assert-failed
// HEAD: result: sat
// HEAD: replay: assert-failed
// HEAD: validate: checked={{[0-9]+}} mismatches=0
// NOSIMP: --vc-fold-top-eqs needs --simplify
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @fold {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 16 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %a = sea.load %m0[%p] : !pm -> i8
  %c7 = sea.const 7 : i8
  %fact = sea.icmp eq, %a, %c7 : i8
  sea.assume %fact
  %v = sea.nondet "v" [2] : i8
  %c4 = sea.const 4 : i64
  %p4 = sea.ptr.add %p, %c4 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%p4], %v inplace : !pm, i8
  %b = sea.load %m1[%p] : !pm -> i8
  %bb = sea.mul %b, %b : i8
  %c49 = sea.const 49 : i8
  %ok = sea.icmp eq, %bb, %c49 : i8
  sea.assert %ok
  sea.halt
}
sea.program @trail {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 16 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %a = sea.load %m0[%p] : !pm -> i8
  %c7 = sea.const 7 : i8
  %fact = sea.icmp eq, %a, %c7 : i8
  sea.assume %fact
  %k = sea.nondet "k" [2] : i64
  %c3 = sea.const 3 : i64
  %kk = sea.and %k, %c3 : i64
  %q = sea.ptr.add %p, %kk : !sea.ptr<64>, i64
  %c9 = sea.const 9 : i8
  %m1 = sea.store %m0[%q], %c9 inplace : !pm, i8
  %b = sea.load %m1[%p] : !pm -> i8
  %ok = sea.icmp eq, %b, %c7 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
