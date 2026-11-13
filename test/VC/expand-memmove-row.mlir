// RUN: sea-vc %s --lower --seam | FileCheck %s --check-prefix=TERM
// RUN: sea-vc %s --lower --seam --expand-memmove=4 | FileCheck %s
// RUN: sed 's/%k1 = sea.icmp eq, %x1, %v1/%k1 = sea.icmp eq, %x1, %v0/' %s | sea-vc - --lower --seam --expand-memmove=4 | FileCheck %s --check-prefix=MUT

// A row copy to a symbolic row: a 4-byte staging row written by two 2-byte
// stores is moved into row r of a 4-row table; a read of row j = r sees the
// row. As one mcopy term (TERM), SeaM cannot decide the range test of a
// read at the symbolic row, and the reads of the staging row at the
// symbolic index are Ackermann residue (this pins that the test exercises
// the case). Expanded (--expand-memmove=4), each copied byte is a load and
// a store SeaM resolves: no Ackermann constraint, the same verdict. The
// mutant (a wrong half expected) stays sat and replays.
// TERM-NOT: vc.expand-memmove
// TERM: vc.ackermann-constraints: {{[1-9]}}
// TERM: result: unsat
// CHECK: vc.expand-memmove: k=4 expanded=1 bytes=4 kept-symbolic=0 kept-over-k=0
// CHECK: vc.ackermann-constraints: 0
// CHECK: result: unsat
// MUT: result: sat
// MUT: replay: assert-failed
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @row {
  %m0 = sea.mem.nondet "m" [1] : !m
  %c16 = sea.const 16 : i64
  %c4 = sea.const 4 : i64
  %c2 = sea.const 2 : i64
  %t = sea.alloca %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %s = sea.alloca %c4 align 4 [3] : i64 -> !sea.ptr<64>
  %true = sea.const 1 : i1
  %v0 = sea.nondet "v0" [4] : i16
  %v1 = sea.nondet "v1" [5] : i16
  %r = sea.nondet "r" [6] : i64
  %j = sea.nondet "j" [7] : i64
  %fr = sea.icmp ult, %r, %c4 : i64
  %fj = sea.icmp ult, %j, %c4 : i64
  %f = sea.and %fr, %fj : i1
  sea.assume %f
  %s2 = sea.ptr.add %s, %c2 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%s], %v0 inplace align 4 : !m, i16
  %m2 = sea.store %m1[%s2], %v1 inplace align 2 : !m, i16
  %off = sea.mul %r, %c4 : i64
  %d = sea.ptr.add %t, %off : !sea.ptr<64>, i64
  %m3 = sea.memmove %m2[%d], %m2[%s], %c4 inplace align 4 src_align 4 : !m
  %joff = sea.mul %j, %c4 : i64
  %e = sea.ptr.add %t, %joff : !sea.ptr<64>, i64
  %e2 = sea.ptr.add %e, %c2 : !sea.ptr<64>, i64
  %x0 = sea.load %m3[%e] align 4 : !m -> i16
  %x1 = sea.load %m3[%e2] align 2 : !m -> i16
  %hit = sea.icmp eq, %j, %r : i64
  %k0 = sea.icmp eq, %x0, %v0 : i16
  %k1 = sea.icmp eq, %x1, %v1 : i16
  %k = sea.and %k0, %k1 : i1
  %nh = sea.xor %hit, %true : i1
  %ok = sea.or %nh, %k : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
