// Stable block ids (sea.block_ids on terminators; sea-vc --reach tags the
// input, the passes keep them). A program without them gets none.
// RUN: split-file %s %t
// RUN: sea-opt --canonicalize %t/merge.mlir | FileCheck %s --check-prefix=MERGE
// A block merged into its only predecessor holds both lists; a cond_br of
// a constant becomes a br with the block's ids; the dead block's ids go.
// MERGE-NOT: sea.br
// MERGE:     sea.halt {sea.block_ids = ["0", "1", "2", "4"]}
// A fold that ends a block early (assume false) keeps the block's ids.
// RUN: sea-opt --canonicalize %t/fold.mlir | FileCheck %s --check-prefix=FOLD
// FOLD: sea.unreachable {sea.block_ids = ["1"]
// An assert of constant false ends its block (sea.halt): the head keeps its
// ids, the erased rest's (here the branch to ^b and ^b) are gone. An assert
// of constant true is erased and the ids stay.
// RUN: sea-opt --canonicalize %t/foldassert.mlir | FileCheck %s --check-prefix=FA
// FA:     sea.assert
// FA-NEXT: sea.halt {sea.block_ids = ["0"]}
// FA-NOT: sea.block_ids
// RUN: sea-opt --canonicalize %t/asserttrue.mlir | FileCheck %s --check-prefix=AT
// AT:     sea.halt {sea.block_ids = ["0"]}
// Reach target: the halts that become sea.unreachable keep their ids; the
// head keeps its ids on the branch to the target's new block.
// RUN: sea-opt --sea-reach-target="id=t.c:5:1" %t/target.mlir | FileCheck %s --check-prefix=RT
// RT:     sea.br ^{{bb[0-9]+}} {sea.block_ids = ["1"]}
// RT:     sea.unreachable {sea.block_ids = ["2"]}
// RT-NOT: sea.halt {sea.block_ids = ["2"]}
// Splits: the head keeps the ids, the tail is a piece `<last>.s<k>`, new
// blocks get `<last>.<role><k>`.
// RUN: sea-opt --sea-split-assert-failures %t/assert.mlir | FileCheck %s --check-prefix=SAF
// SAF:      sea.cond_br {{.*}} {sea.block_ids = ["0"]}
// SAF:      sea.br ^[[C:bb[0-9]+]] {sea.block_ids = ["0.p0"]}
// SAF:      sea.br ^[[C]] {sea.block_ids = ["0.q0"]}
// SAF:      sea.halt {sea.block_ids = ["0.s0"]}
// RUN: sea-opt --sea-merge-asserts %t/two.mlir | FileCheck %s --check-prefix=MA
// MA:      sea.cond_br {{.*}} {sea.block_ids = ["0"]}
// MA:      sea.br ^[[X:bb[0-9]+]](%{{.*}}) {sea.block_ids = ["0.f0"]}
// MA:      sea.cond_br {{.*}} {sea.block_ids = ["0.s0"]}
// MA:      sea.br ^[[X]](%{{.*}}) {sea.block_ids = ["0.s0.f0"]}
// MA:      sea.br ^[[X]](%{{.*}}) {sea.block_ids = ["0.s0.s0"]}
// MA:      sea.halt {sea.block_ids = ["x"]}
// RUN: sea-opt --sea-split-critical-edges %t/edge.mlir | FileCheck %s --check-prefix=EDGE
// EDGE: sea.br ^{{bb[0-9]+}} {sea.block_ids = ["0>2"]}
// RUN: sea-opt --sea-split-at-checks %t/fold.mlir | FileCheck %s --check-prefix=SAC
// SAC: sea.br ^{{bb[0-9]+}} {sea.block_ids = ["1"]}
// SAC: sea.br ^{{bb[0-9]+}} {sea.block_ids = ["1.s0"]}
// RUN: sea-opt --pass-pipeline='builtin.module(sea.program(sea-expand-memset{max-length=2}))' %t/memset.mlir | FileCheck %s --check-prefix=MS
// MS:     sea.br ^{{bb[0-9]+}}(%{{.*}}) {sea.block_ids = ["0"]}
// MS-DAG: {sea.block_ids = ["0.m0"]}
// MS-DAG: {sea.block_ids = ["0.m4"]}
// MS:     sea.halt {sea.block_ids = ["0.s0"]}
// RUN: sea-opt --sea-guard-null-memhavoc %t/havoc.mlir | FileCheck %s --check-prefix=GN
// GN: sea.cond_br {{.*}} {sea.block_ids = ["0"]}
// GN: sea.br ^{{bb[0-9]+}}(%{{.*}}) {sea.block_ids = ["0.h0"]}
// GN: sea.halt {sea.block_ids = ["0.s0"]}
// A program without ids: the passes add none.
// RUN: sea-opt --sea-merge-asserts --sea-split-assert-failures --sea-split-critical-edges --canonicalize %t/plain.mlir | FileCheck %s --check-prefix=PLAIN
// PLAIN-NOT: sea.block_ids

//--- merge.mlir
sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %t = sea.const true
  sea.br ^a {sea.block_ids = ["0"]}
^a:
  sea.cond_br %t, ^b, ^d {sea.block_ids = ["1"]}
^b:
  %c = sea.icmp ne, %x, %x : i8
  sea.assume %c
  sea.br ^j {sea.block_ids = ["2"]}
^d:
  sea.br ^j {sea.block_ids = ["3"]}
^j:
  %f = sea.const false
  sea.assert %f
  sea.halt {sea.block_ids = ["4"]}
}

//--- fold.mlir
sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %f = sea.const false
  %c = sea.icmp ult, %x, %x : i8
  sea.cond_br %c, ^a, ^b {sea.block_ids = ["0"]}
^a:
  sea.assume %f
  sea.br ^b {sea.block_ids = ["1"]}
^b:
  sea.assert %f
  sea.halt {sea.block_ids = ["2"]}
}

//--- foldassert.mlir
sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %f = sea.const false
  sea.assert %f
  %c = sea.icmp ult, %x, %x : i8
  sea.assume %c
  sea.br ^b {sea.block_ids = ["0"]}
^b:
  sea.halt {sea.block_ids = ["1"]}
}

//--- asserttrue.mlir
sea.program @main {
  %t = sea.const true
  sea.assert %t
  sea.halt {sea.block_ids = ["0"]}
}

//--- target.mlir
sea.program @main {
  %x = sea.nondet "x" [0] : i8 loc("t.c":2:1)
  %c = sea.icmp ult, %x, %x : i8 loc("t.c":2:2)
  sea.cond_br %c, ^a, ^b {sea.block_ids = ["0"]} loc("t.c":2:3)
^a:
  sea.assume %c loc("t.c":5:1)
  sea.halt {sea.block_ids = ["1"]}
^b:
  sea.halt {sea.block_ids = ["2"]}
}

//--- assert.mlir
sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %c = sea.icmp ult, %x, %x : i8
  sea.assert %c {sea.reach_site = 0 : i64}
  sea.halt {sea.block_ids = ["0"]}
}

//--- two.mlir
sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %c = sea.icmp ult, %x, %x : i8
  sea.assert %c
  sea.assert %c
  sea.halt {sea.block_ids = ["0"]}
}

//--- edge.mlir
sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %c = sea.icmp ult, %x, %x : i8
  sea.cond_br %c, ^a, ^j {sea.block_ids = ["0"]}
^a:
  sea.br ^j {sea.block_ids = ["1"]}
^j:
  sea.assert %c
  sea.halt {sea.block_ids = ["2"]}
}

//--- memset.mlir
sea.program @main {
  %m0 = sea.mem.nondet "r" [0] : !sea.mem<!sea.addr<8>, i8>
  %n = sea.nondet "n" [1] : i8
  %p = sea.const 16 : i8
  %pa = sea.addr.from_int %p : i8 to !sea.addr<8>
  %v = sea.const 7 : i8
  %m1 = sea.memset %m0[%pa], %v, %n inplace : !sea.mem<!sea.addr<8>, i8>
  %x = sea.load %m1[%pa] : !sea.mem<!sea.addr<8>, i8> -> i8
  %ok = sea.icmp ne, %x, %v : i8
  sea.assert %ok
  sea.halt {sea.block_ids = ["0"]}
}

//--- havoc.mlir
!m = !sea.mem<!sea.ptr<16>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i16
  %a = sea.malloc %c4 align 1 [1] : i16 -> !sea.ptr<16>
  %n = sea.ptr.null : !sea.ptr<16>
  %c = sea.nondet "c" [2] : i1
  %p = sea.select %c, %a, %n : !sea.ptr<16>
  %m1 = sea.memhavoc %m0[%p], %c4 [3] inplace : !m
  %x = sea.load %m1[%a] : !m -> i8
  %z = sea.const 0 : i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt {sea.block_ids = ["0"]}
}

//--- plain.mlir
sea.program @main {
  %x = sea.nondet "x" [0] : i8
  %c = sea.icmp ult, %x, %x : i8
  sea.cond_br %c, ^a, ^j
^a:
  sea.assert %c
  sea.br ^j
^j:
  sea.assert %c
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
