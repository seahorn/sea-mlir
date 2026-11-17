// --narrow-offsets narrows read addresses only: a pointer compare in the
// program is not an access and keeps its 64-bit meaning. q = p + n with
// n <= 16 for a 16-byte object (k = 4): q is one past the end for n = 16,
// and q == p only for n = 0 (at 4 bits, 16 and 0 would be equal). The read
// at p + i is narrowed; the compare is not, and the assertion holds.
// RUN: sea-vc %s --lower --seam --narrow-offsets 2>&1 | FileCheck %s
// CHECK: vc.narrow-reads: 2
// CHECK: vc.narrow-k-max: 4
// CHECK: result: unsat
// The compare is q - p = 0 over 64 bits (SeaM cancels the base).
// RUN: sea-vc %s --lower --seam --narrow-offsets --smt2 2>/dev/null | FileCheck %s --check-prefix=SMT
// SMT: (= nd.3_0 #x0000000000000000)
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ope {
  %m = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %p = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %ilt = sea.icmp ult, %i, %c16 : i64
  sea.assume %ilt
  %pi = sea.ptr.add %p, %i : !sea.ptr<64>, i64
  %x = sea.load %m[%pi] : !m -> i8
  %x0 = sea.load %m[%p] : !m -> i8
  %n = sea.nondet "n" [3] : i64
  %nle = sea.icmp ule, %n, %c16 : i64
  sea.assume %nle
  %q = sea.ptr.add %p, %n : !sea.ptr<64>, i64
  %same = sea.ptr.cmp eq, %q, %p : !sea.ptr<64>
  %end = sea.icmp eq, %n, %c16 : i64
  %bad0 = sea.and %same, %end : i1
  %xe = sea.icmp eq, %x, %x0 : i8
  %bad = sea.and %bad0, %xe : i1
  %t = sea.const 1 : i1
  %ok = sea.xor %bad, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
