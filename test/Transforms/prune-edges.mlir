// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-prune-edges{fold=false canonicalize=false cse=false}))' --mlir-print-debuginfo | FileCheck %s
// RUN: sea-opt %s --sea-prune-edges | FileCheck %s --check-prefix=FIX
// sea-prune-edges alone (one round, no clean-up): which edges are dead and
// what replaces them (docs/design/PruneEdges.md).

// @chain: the constant 0 entering ^a makes `ret != 0` false; the cond_br
// becomes an assume of the other side and a br; the other edge stays.
// CHECK-LABEL: sea.program @chain
// CHECK-SAME: "by-args" = 1
// CHECK-NOT: sea.cond_br
// CHECK: %[[N:.*]] = sea.xor %{{.*}}, %{{.*}} : i1
// CHECK: sea.assume %[[N]]
// CHECK: sea.br ^[[A:bb[0-9]+]](%{{.*}} : i32)
// CHECK: ^[[A]](
// CHECK: sea.assume
sea.program @chain {
  %z = sea.const 0 : i32
  %x = sea.nondet "x" [0] : i32
  %c = sea.nondet "c" [1] : i1
  sea.cond_br %c, ^a(%z : i32), ^a(%x : i32)
^a(%r : i32):
  %ne = sea.icmp ne, %r, %z : i32
  sea.assume %ne
  %g = sea.icmp eq, %r, %x : i32
  sea.assert %g
  sea.halt
}

// @nonconst: no constant reaches the join: nothing to prune.
// CHECK-LABEL: sea.program @nonconst
// CHECK-NOT: sea.prune_edges
// CHECK: sea.cond_br
sea.program @nonconst {
  %x = sea.nondet "x" [0] : i1
  %y = sea.nondet "y" [1] : i1
  %c = sea.nondet "c" [2] : i1
  sea.cond_br %c, ^j(%x : i1), ^j(%y : i1)
^j(%ok : i1):
  sea.assume %ok
  sea.assert %ok
  sea.halt
}

// @br: a straight block whose br is dead ends in sea.unreachable; the
// assert before it stays (it may fail).
// CHECK-LABEL: sea.program @br
// CHECK: sea.assert
// CHECK-NEXT: sea.unreachable
sea.program @br {
  %f = sea.const 0 : i1
  %c = sea.nondet "c" [0] : i1
  sea.assert %c
  sea.br ^j(%f : i1)
^j(%ok : i1):
  sea.assume %ok
  sea.halt
}

// @held: a merged assert's branch (location `sea.assert`) whose landing
// edge is dead: the br is no failure site, without the marker and the id.
// CHECK-LABEL: sea.program @held
// CHECK-NOT: sea.xor
// CHECK: sea.assume %0
// CHECK: sea.br ^{{bb[0-9]+}} loc(#[[UNK:loc[0-9]*]])
// CHECK-NOT: sea.assert_id
// CHECK-LABEL: sea.program @failed
// CHECK: sea.br ^{{bb[0-9]+}} {sea.assert_id = 3 : i64} loc(#[[M:loc[0-9]+]])
sea.program @held {
  %x = sea.nondet "x" [0] : i1
  sea.cond_br %x, ^cont, ^land {sea.assert_id = 3 : i64} loc("sea.assert"("x.c":3:3))
^cont:
  sea.assume %x
  sea.assert %x
  sea.halt
^land:
  sea.unreachable
}

// @failed: its continuation is dead: the br into the landing block is the
// failure, marker and id kept.
sea.program @failed {
  %x = sea.nondet "x" [0] : i1
  sea.cond_br %x, ^cont, ^land {sea.assert_id = 3 : i64} loc("sea.assert"("x.c":3:3))
^cont:
  sea.unreachable
^land:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// @loop: a program with a cycle is not BMC form: left alone (sea-vc
// refuses it), also a cycle that never reaches a check.
// CHECK-LABEL: sea.program @loop
// CHECK-NOT: sea.prune_edges
// CHECK: sea.cond_br
sea.program @loop {
  %c = sea.nondet "c" [0] : i1
  sea.cond_br %c, ^l, ^e
^l:
  %d = sea.nondet "d" [1] : i1
  sea.cond_br %d, ^l, ^l2
^l2:
  sea.br ^l
^e:
  sea.assert %c
  sea.halt
}

// The marker locations of @held / @failed.
// CHECK-DAG: #[[UNK]] = loc(unknown)
// CHECK-DAG: #[[M]] = loc("sea.assert"(

// The default rounds (assume-fold, canonicalize, cse): in @chain the new
// assume of !c is the only one left on %c; the join merges into the entry.
// FIX-LABEL: sea.program @chain
// FIX-NOT: sea.cond_br
// FIX-NOT: ^bb
// FIX: sea.halt

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
