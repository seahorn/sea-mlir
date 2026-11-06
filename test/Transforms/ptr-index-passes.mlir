// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-lower-fat-pointers,sea-place-static))' | FileCheck %s --check-prefix=PLACE
// RUN: sea-opt %s --sea-widen-memory="k=8 report=true" -o /dev/null 2>&1 | FileCheck %s --check-prefix=WIDEN
// RUN: sea-opt %s --sea-drop-fresh-memhavoc | FileCheck %s --check-prefix=DROP
// RUN: sea-vc %s --program=place --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=place --lower --seam | FileCheck %s --check-prefix=UNSAT

// sea.ptr.index through the passes before the VC (PtrIndex.md):
// - placement keeps the op, on the base's address, with its flags;
// - fat pointers: ptr.base of p[i].f is p (offset companion + the op's
//   offset), so @place's assert (base(q) == p) holds;
// - widen-memory: an 8-aligned base, stride 8 and offset 0 prove the
//   access aligned;
// - drop-fresh-memhavoc: a havoc through p[i] of a fresh object is dropped.
// PLACE: sea.segment "heap.1"
// PLACE: sea.ptr.index %{{.*}}[%{{.*}}] strides [8] offset 4 inbounds : !sea.addr<64>, i64
// PLACE-NOT: sea.ptr.base
// UNSAT: result: unsat
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @place {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 64 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %c7 = sea.const 7 : i64
  %iok = sea.icmp ult, %i, %c7 : i64
  sea.assume %iok
  %q = sea.ptr.index %p [%i] strides [8] offset 4 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] : !pm -> i8
  %b = sea.ptr.base %q : !sea.ptr<64>
  %same = sea.ptr.cmp eq, %b, %p : !sea.ptr<64>
  sea.assert %same
  sea.halt
}

// WIDEN: remark: {{.*}}memory widened to 8-byte words
!wm = !sea.mem<!sea.ptr<64>, i8>
sea.program @widen {
  %m0 = sea.mem.nondet "m" [0] : !wm
  %n = sea.const 64 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %q = sea.ptr.index %p [%i] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] align 8 : !wm -> i64
  %v = sea.const 5 : i64
  %m1 = sea.store %m0[%q], %v inplace align 8 : !wm, i64
  sea.halt
}

// DROP-LABEL: sea.program @fresh
// DROP-NOT: sea.memhavoc
sea.program @fresh {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 64 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %q = sea.ptr.index %p [%i] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %c8 = sea.const 8 : i64
  %m1 = sea.memhavoc %m0[%q], %c8 [3] inplace : !pm
  %x = sea.load %m1[%p] : !pm -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
