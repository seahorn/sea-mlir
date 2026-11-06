// A-INB with a symbolic offset: the store address a + n may land in b (n is
// unconstrained), which the static check cannot see. --seam takes the access
// to stay in a, skips it when reading b, and answers unsat where the plain VC
// is sat: the documented conditional answer (A-INB assumed and reported, not
// discharged). `seam.inb: assumed` says what the answer rests on.
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --seam --seam-inb=0 | FileCheck %s --check-prefix=NOINB
// RUN: sea-vc %s --lower --seam | FileCheck %s --check-prefix=SEAM
// RUN: not sea-vc %s --lower --seam --seam-check-inb 2>&1 | FileCheck %s --check-prefix=GONE
// PLAIN: result: sat
// NOINB: seam.inb: off
// NOINB: result: sat
// SEAM: seam.inb: assumed accesses={{[0-9]+}} conditions={{[1-9][0-9]*}}
// SEAM: result: unsat
// GONE: Unknown command line argument '--seam-check-inb'
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @sym {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [3] : i64
  %v7 = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %z inplace : !m, i8
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %m2 = sea.store %m1[%p], %v7 inplace : !m, i8
  %x = sea.load %m2[%b] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
