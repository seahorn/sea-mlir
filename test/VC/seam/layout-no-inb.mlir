// Without A-INB (--seam-inb=0, also the vc.seam-inb-violated fallback),
// the address compares of accesses and the memop range tests between two
// placed objects at numeral offsets are decided by the placed numerals
// (cmp.layout, mem.in-layout; uses: LAYOUT, the equations obj = n of the
// VC). Before them nothing separated two objects without A-INB, and every
// undecided memmove row nested the next read in the dynamic-shape case.
// @store: a store to B[0] between a store to A[1] and its reload.
// @copy: a memmove into B between a store to A[1] and its reload.
// @program: a program compare A + 4 == B is never decided by the layout
// (SeaM-null.md, point 4): it stays for the solver.
// RUN: sea-vc %s --lower --program=store --seam --seam-inb=0 --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,STORE
// RUN: sea-vc %s --lower --program=copy --seam --seam-inb=0 --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,COPY
// RUN: sea-vc %s --lower --program=store --seam --seam-inb=0 --seam-check 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: sea-vc %s --lower --program=copy --seam --seam-inb=0 --seam-check 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHK
// RUN: sea-vc %s --lower --program=store --seam --seam-inb=0 --seam-layout=0 --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,OFF
// RUN: sea-vc %s --lower --program=copy --seam --seam-inb=0 --seam-layout=0 --seam-stats 2>&1 | FileCheck %s --check-prefixes=UNSAT,OFF
// RUN: sea-vc %s --lower --program=program --seam --seam-inb=0 --seam-stats 2>&1 | FileCheck %s --check-prefixes=PROG
// STORE: seam.rule.cmp.layout: {{[1-9]}}
// COPY: seam.rule.mem.in-layout: {{[1-9]}}
// OFF-NOT: seam.rule.cmp.layout
// OFF-NOT: seam.rule.mem.in-layout
// PROG-NOT: seam.rule.cmp.layout
// UNSAT: result: unsat
// PROG: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @store {
  %m0 = sea.mem.nondet "region.0" [0] : !m
  %c1 = sea.const 1 : i64
  %c8 = sea.const 8 : i64
  %A = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %a1 = sea.ptr.add %A, %c1 : !sea.ptr<64>, i64
  %v = sea.nondet "v" [3] : i8
  %w = sea.nondet "w" [4] : i8
  %m1 = sea.store %m0[%a1], %v inplace align 1 : !m, i8
  %m2 = sea.store %m1[%B], %w inplace align 1 : !m, i8
  %r = sea.load %m2[%a1] align 1 : !m -> i8
  %ok = sea.icmp eq, %r, %v : i8
  sea.assert %ok
  sea.halt
}
sea.program @copy {
  %m0 = sea.mem.nondet "region.0" [0] : !m
  %c1 = sea.const 1 : i64
  %c4 = sea.const 4 : i64
  %c8 = sea.const 8 : i64
  %A = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %C = sea.alloca %c8 align 8 [3] : i64 -> !sea.ptr<64>
  %a1 = sea.ptr.add %A, %c1 : !sea.ptr<64>, i64
  %v = sea.nondet "v" [4] : i8
  %m1 = sea.store %m0[%a1], %v inplace align 1 : !m, i8
  %m2 = sea.memmove %m1[%B], %m1[%C], %c4 inplace : !m
  %r = sea.load %m2[%a1] align 1 : !m -> i8
  %ok = sea.icmp eq, %r, %v : i8
  sea.assert %ok
  sea.halt
}
sea.program @program {
  %c4 = sea.const 4 : i64
  %c8 = sea.const 8 : i64
  %A = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %a4 = sea.ptr.add %A, %c4 : !sea.ptr<64>, i64
  %eq = sea.ptr.cmp eq, %a4, %B : !sea.ptr<64>
  %t = sea.const 1 : i1
  %ok = sea.xor %eq, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
