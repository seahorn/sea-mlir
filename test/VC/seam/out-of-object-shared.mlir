// The static out-of-object report is pairwise (A-SEP is pairwise): a and b
// are 4-byte objects 16 bytes apart (align 16); a + 8 and b - 8 are the
// same cell of the gap between them. @shared: a store through b - 8, a
// store through a + 8 (the same cell), a load through b - 8: each access
// lands in no object, yet two provenances write one cell, so A-SEP fails
// when they execute: lands=shared, naming the other access, and the
// warning says A-SEP is broken (plain: sat; --seam: unsat under a broken
// premise). @gap: the same cell through b only (--lower-opt=false keeps
// the load): one provenance, lands=gap,
// and the warning says the gap accesses keep A-SEP and A-NULL. @loads: two
// loads of the cell through a and b: no write, so no shared pair.
// RUN: sea-vc %s --program=shared --lower | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --program=shared --lower --seam 2>&1 | FileCheck %s --check-prefix=SHARED
// RUN: sea-vc %s --program=gap --lower --lower-opt=false --seam 2>&1 | FileCheck %s --check-prefix=GAP
// RUN: sea-vc %s --program=loads --lower --seam 2>&1 | FileCheck %s --check-prefix=LOADS
// PLAIN: result: sat
// SHARED: Warning: 3 access(es) {{.*}}shared=3{{.*}}A-SEP is broken if {{.*}}both of a shared pair{{.*}}[vc.seam-out-of-object]
// SHARED: seam.out-of-object-at: {{.*}}out-of-object-shared.mlir:[[@LINE+19]]:{{[0-9]+}} store: {{.*}} lands=shared with={{.*}}out-of-object-shared.mlir:[[@LINE+20]]:{{[0-9]+}}
// SHARED: seam.out-of-object: accesses=3 object=0 shared=3 unknown=0 null=0 gap=0
// SHARED: result: unsat
// GAP: Warning: {{.*}}gap=2); the premises stay; the gap ones keep A-SEP and A-NULL{{.*}}[vc.seam-out-of-object]
// GAP-NOT: lands=shared
// GAP: seam.out-of-object: accesses=2 object=0 shared=0 unknown=0 null=0 gap=2
// LOADS: seam.out-of-object: accesses=2 object=0 shared=0 unknown=0 null=0 gap=2
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @shared {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %a = sea.malloc %c4 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c4 align 16 [2] : i64 -> !sea.ptr<64>
  %off = sea.const 8 : i64
  %moff = sea.const -8 : i64
  %pa = sea.ptr.add %a, %off : !sea.ptr<64>, i64
  %pb = sea.ptr.add %b, %moff : !sea.ptr<64>, i64
  %z = sea.const 0 : i8
  %n = sea.const 9 : i8
  %m1 = sea.store %m0[%pb], %z inplace : !m, i8
  %m2 = sea.store %m1[%pa], %n inplace : !m, i8
  %x = sea.load %m2[%pb] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}
sea.program @gap {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %a = sea.malloc %c4 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c4 align 16 [2] : i64 -> !sea.ptr<64>
  %moff = sea.const -8 : i64
  %pb = sea.ptr.add %b, %moff : !sea.ptr<64>, i64
  %z = sea.const 0 : i8
  %m1 = sea.store %m0[%pb], %z inplace : !m, i8
  %x = sea.load %m1[%pb] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}
sea.program @loads {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %a = sea.malloc %c4 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c4 align 16 [2] : i64 -> !sea.ptr<64>
  %off = sea.const 8 : i64
  %moff = sea.const -8 : i64
  %pa = sea.ptr.add %a, %off : !sea.ptr<64>, i64
  %pb = sea.ptr.add %b, %moff : !sea.ptr<64>, i64
  %x = sea.load %m0[%pa] : !m -> i8
  %y = sea.load %m0[%pb] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
