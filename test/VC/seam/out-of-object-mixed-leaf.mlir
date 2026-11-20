// A gap cell of a flagged access, also reached by an ite leaf of an access
// that is not flagged. A = [4096, 4104), B = [4112, 4120), the gap between
// them [4104, 4112). The store through A + 12 = 4108 leaves A on its only
// alternative (flagged). The load through select(c, B - 4, B) has a leaf in
// B, so it is not flagged, but its leaf B - 4 = 4108 is the same cell, read
// through B: for c = 1 two provenances access one cell, a write and a read,
// and A-SEP fails. The report pairs the gap cell with that leaf: the store
// lands shared, naming the load, and the load is not reported itself.
// @mvsrc: a flagged load of the gap cell and a memmove whose source is
// select(c, B - 4, B): two reads, so no shared pair (the leaf of a
// memmove's source is a read).
// RUN: sea-vc %s --program=mixed | FileCheck %s --check-prefix=PLAIN
// RUN: sea-run %s --program=mixed --input=1=1 | FileCheck %s --check-prefix=INTERP
// RUN: sea-vc %s --program=mixed --seam 2>&1 | FileCheck %s
// RUN: sea-vc %s --program=mvsrc --seam 2>&1 | FileCheck %s --check-prefix=MVSRC
// PLAIN: result: sat
// INTERP: status: assert-failed
// CHECK: Warning: 1 access(es) {{.*}}shared=1{{.*}}A-SEP may break if {{.*}}[vc.seam-out-of-object]
// CHECK-NOT: lands=gap
// CHECK: seam.out-of-object-at: {{.*}}out-of-object-mixed-leaf.mlir:[[@LINE+17]]:{{[0-9]+}} store: {{.*}} lands=shared with={{.*}}out-of-object-mixed-leaf.mlir:[[@LINE+18]]:{{[0-9]+}}
// CHECK: seam.out-of-object: accesses=1 object=0 shared=1 unknown=0 null=0 gap=0
// MVSRC-NOT: lands=shared
// MVSRC: seam.out-of-object: accesses=1 object=0 shared=0 unknown=0 null=0 gap=1
!m = !sea.mem<!sea.addr<64>, i8>
sea.program @mixed {
  %A = sea.segment "heap.1" base 4096 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %B = sea.segment "heap.2" base 4112 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c = sea.nondet "c" [1] : i1
  %c12 = sea.const 12 : i64
  %cm4 = sea.const -4 : i64
  %p = sea.addr.add %A, %c12 : !sea.addr<64>, i64
  %qb = sea.addr.add %B, %cm4 : !sea.addr<64>, i64
  %q = sea.select %c, %qb, %B : !sea.addr<64>
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %x = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}
sea.program @mvsrc {
  %A = sea.segment "heap.1" base 4096 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %B = sea.segment "heap.2" base 4112 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %C = sea.segment "heap.3" base 4128 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [2] : !m
  %c = sea.nondet "c" [1] : i1
  %c12 = sea.const 12 : i64
  %cm4 = sea.const -4 : i64
  %one = sea.const 1 : i64
  %p = sea.addr.add %A, %c12 : !sea.addr<64>, i64
  %qb = sea.addr.add %B, %cm4 : !sea.addr<64>, i64
  %q = sea.select %c, %qb, %B : !sea.addr<64>
  %x = sea.load %m0[%p] : !m -> i8
  %m1 = sea.memmove %m0[%C], %m0[%q], %one inplace : !m
  %y = sea.load %m1[%C] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
