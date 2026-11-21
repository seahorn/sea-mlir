// The premise mark (sea.premise, placement's symbolic-size=assert) follows
// the assert it marks: sea-merge-asserts copies it onto both branches it
// makes of the assert; a branch whose assert held is no failure site and
// loses it (canonicalize, sea-prune-edges).
//
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-merge-asserts))' -split-input-file 2>/dev/null | FileCheck %s --check-prefix=MERGE
// MERGE-LABEL: sea.program @merge
// MERGE: sea.cond_br {{.*}} {sea.premise = "symbolic-size-bound"}
// MERGE: sea.br {{.*}} {sea.premise = "symbolic-size-bound"}
// MERGE-NOT: sea.premise
// MERGE-LABEL: sea.program @held
//
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(canonicalize))' -split-input-file 2>/dev/null | FileCheck %s --check-prefix=CANON
// CANON-LABEL: sea.program @held
// CANON-NOT: sea.premise
// CANON: sea.br ^bb{{[0-9]+}}(%{{[0-9]+}} : i8){{$}}
// CANON-NOT: sea.premise
// CANON-LABEL: sea.program @pruned
//
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-prune-edges{canonicalize=false cse=false fold=false}))' -split-input-file 2>/dev/null | FileCheck %s --check-prefix=PRUNE
// PRUNE-LABEL: sea.program @pruned
// PRUNE: sea.assume
// PRUNE-NEXT: sea.br ^bb{{[0-9]+}}{{$}}

sea.program @merge {
  %k = sea.nondet "k" [1] : i64
  %c = sea.const 4096 : i64
  %fits = sea.icmp ule, %k, %c : i64
  sea.assert %fits {sea.premise = "symbolic-size-bound"} loc("a.c":3:7)
  %j = sea.nondet "j" [2] : i1
  sea.assert %j loc("a.c":9:3)
  sea.halt
}

// -----

// a merged premise assert of constant 1 (held) on a path that keeps its
// branch (^c has two predecessors)
sea.program @held {
  %t = sea.const 1 : i1
  %f = sea.const 0 : i1
  %n = sea.nondet "n" [1] : i1
  sea.cond_br %n, ^a, ^b
^a:
  %u = sea.nondet "u" [2] : i8
  sea.cond_br %t, ^c(%u : i8), ^l {sea.premise = "symbolic-size-bound"} loc("sea.assert"("a.c":3:7))
^b:
  %v = sea.nondet "v" [3] : i8
  sea.br ^c(%v : i8)
^c(%w : i8):
  sea.print "w" %w : i8
  sea.br ^x(%t : i1)
^l:
  sea.br ^x(%f : i1) {sea.premise = "symbolic-size-bound"} loc("sea.assert"("a.c":3:7))
^x(%ok : i1):
  sea.assert %ok
  sea.halt
}

// -----

// the landing edge passes false to an assume: dead, so the premise held on
// every path that goes on
sea.program @pruned {
  %k = sea.nondet "k" [1] : i64
  %c = sea.const 4096 : i64
  %fits = sea.icmp ule, %k, %c : i64
  %t = sea.const 1 : i1
  %f = sea.const 0 : i1
  sea.cond_br %fits, ^c, ^l {sea.premise = "symbolic-size-bound"} loc("sea.assert"("a.c":3:7))
^c:
  sea.br ^x(%t : i1)
^l:
  sea.br ^x(%f : i1) {sea.premise = "symbolic-size-bound"} loc("sea.assert"("a.c":3:7))
^x(%ok : i1):
  sea.assume %ok
  %j = sea.nondet "j" [2] : i1
  sea.assert %j
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
