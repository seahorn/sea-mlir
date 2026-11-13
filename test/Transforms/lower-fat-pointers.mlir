// RUN: sea-opt %s --sea-lower-fat-pointers | FileCheck %s --check-prefix=LOW
// RUN: sea-opt %s --sea-lower-fat-pointers | sea-run - --missing=zero | FileCheck %s --check-prefix=INT
// RUN: sea-opt %s --sea-lower-fat-pointers --sea-place-static | sea-run - --missing=zero | FileCheck %s --check-prefix=INT
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VC
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=COMP
// RUN: sea-vc %s --lower --seam --seam-stats --expand-memmove=8 2>&1 | FileCheck %s --check-prefix=EXP
// RUN: sea-vc %s --lower --seam --seam-stats --widen-memory=8 --expand-memmove=8 2>&1 | FileCheck %s --check-prefix=WIDEX

// A pointer's offset and size travel with it through memory: stored,
// copied by memmove, loaded back, then asked about. Offsets and sizes are
// companion values; pointer-holding memory gets companion memories.
// The companion memories are keyed (a pointer-width cell at every byte
// address, never split):
// LOW: sea.mem.nondet "fat.offset" {{.*}} : !sea.mem<!sea.ptr<64>, i64, keyed>
// With --seam, the companion accesses are mirrored by the data accesses at
// the same addresses: exempt from A-INB accounting, none unmirrored.
// COMP-NOT: unmirrored
// COMP: seam.rule.companion.access: 8
// COMP-NOT: unmirrored
// sea-expand-small-memmove (--expand-memmove) expands a companion memmove
// only with its data memmove: all three (EXP), or none when widening makes
// the data memmove a word copy, which is kept (WIDEX); none unmirrored.
// EXP-NOT: unmirrored
// EXP: vc.expand-memmove: k=8 expanded=3 bytes=24
// EXP-NOT: unmirrored
// EXP: result: unsat
// WIDEX-NOT: unmirrored
// WIDEX: vc.expand-memmove: k=8 expanded=0 {{.*}} kept-word-layout=1 {{.*}} kept-companion-mirror=2
// WIDEX-NOT: unmirrored
// WIDEX: result: unsat
// LOW-NOT: sea.ptr.dereferenceable
// LOW-NOT: sea.ptr.base
// LOW: sea.memmove
// LOW: sea.memmove
// LOW: sea.memmove
// INT: status: halted
// VC: result: unsat

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @through_memory {
  %m0 = sea.mem.nondet "m" [0] : !m
  %eight = sea.const 8 : i64
  %sixteen = sea.const 16 : i64
  %buf = sea.alloca %sixteen align 8 [1] : i64 -> !sea.ptr<64>
  %cells = sea.alloca %sixteen align 8 [2] : i64 -> !sea.ptr<64>
  %five = sea.const 5 : i64
  %p = sea.ptr.add %buf, %five : !sea.ptr<64>, i64
  // store p at cells[0], copy to cells[8], load it back from there
  %m1 = sea.store %m0[%cells], %p inplace : !m, !sea.ptr<64>
  %slot = sea.ptr.add %cells, %eight : !sea.ptr<64>, i64
  %m2 = sea.memmove %m1[%slot], %m1[%cells], %eight inplace : !m
  %q = sea.load %m2[%slot] : !m -> !sea.ptr<64>
  // [q, q+11) fits (5 + 11 = 16); [q, q+12) does not; base(q) = buf
  %eleven = sea.const 11 : i64
  %twelve = sea.const 12 : i64
  %a = sea.ptr.dereferenceable %q, %eleven : !sea.ptr<64>, i64
  %b = sea.ptr.dereferenceable %q, %twelve : !sea.ptr<64>, i64
  %base = sea.ptr.base %q : !sea.ptr<64>
  %c = sea.ptr.cmp eq, %base, %buf : !sea.ptr<64>
  %t = sea.const 1 : i1
  %nb = sea.xor %b, %t : i1
  %r1 = sea.and %a, %nb : i1
  %ok = sea.and %r1, %c : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
