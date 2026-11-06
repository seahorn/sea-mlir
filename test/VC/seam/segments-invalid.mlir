// --seam names each object base by its segment and needs the objects disjoint:
// two segments with one name would make the side equalities contradict
// (every VC unsat), overlapping ones make "different objects differ" wrong.
// Both are errors, not a silent answer. Without --seam the VC is plain
// numerals and both programs are fine.
// RUN: sea-vc %s --program=dup | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --program=ovl | FileCheck %s --check-prefix=PLAIN
// RUN: not sea-vc %s --program=dup --seam 2>&1 | FileCheck %s --check-prefix=DUP
// RUN: not sea-vc %s --program=ovl --seam 2>&1 | FileCheck %s --check-prefix=OVL
// PLAIN: result: sat
// DUP: Error: two sea.segment ops are named "heap.1"{{.*}}[vc.seam-segment-duplicate]
// OVL: Error: sea.segment "heap.2" overlaps "heap.1"{{.*}}[vc.seam-segment-overlap]
sea.program @dup {
  %a = sea.segment "heap.1" base 4096 : i64 size 16 : i64 align 16 kind "heap" : !sea.addr<64>
  %b = sea.segment "heap.1" base 4112 : i64 size 16 : i64 align 16 kind "heap" : !sea.addr<64>
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}
!am = !sea.mem<!sea.addr<64>, i8>
sea.program @ovl {
  %a = sea.segment "heap.1" base 4096 : i64 size 16 : i64 align 8 kind "heap" : !sea.addr<64>
  %b = sea.segment "heap.2" base 4104 : i64 size 16 : i64 align 8 kind "heap" : !sea.addr<64>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %v7 = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %v7 inplace : !am, i8
  %c8 = sea.const 8 : i64
  %p = sea.addr.add %a, %c8 : !sea.addr<64>, i64
  %x = sea.load %m1[%p] : !am -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
