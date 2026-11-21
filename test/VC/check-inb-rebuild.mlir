// The manifest's `discharge` object holds the job-level inputs of the
// check-inb.discharges line; with the parts' outputs it rebuilds that line
// (docs/design/CheckInb.md, "Rebuilding the discharges line"), as an
// outside runner would.
// RUN: rm -rf %t.a %t.b %t.c %t.d
// RUN: sea-vc %s --program=sym --check-inb --lower --seam --ackermannize=off --check-inb-dir=%t.a > %t.a.out 2>/dev/null
// RUN: %python %S/Inputs/rebuild-discharges.py %t.a %t.a.out | FileCheck %s
// RUN: sea-vc %s --program=mhnull --check-inb --lower --seam --ackermannize=off --check-inb-dir=%t.b > %t.b.out 2>/dev/null
// RUN: %python %S/Inputs/rebuild-discharges.py %t.b %t.b.out | FileCheck %s
// RUN: sea-vc %s --program=oob --check-inb --lower --seam --ackermannize=off --check-inb-dir=%t.c > %t.c.out 2>/dev/null
// RUN: %python %S/Inputs/rebuild-discharges.py %t.c %t.c.out | FileCheck %s
// RUN: sea-vc %s --program=big --check-inb --lower --seam --ackermannize=off --check-inb-dir=%t.d > %t.d.out 2>/dev/null
// RUN: %python %S/Inputs/rebuild-discharges.py %t.d %t.d.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=MANIFEST < %t.d/manifest.json

// CHECK: discharges-match
// MANIFEST: "discharge": {
// MANIFEST-NEXT: "largest_object": "2147483648",
// MANIFEST-NEXT: "nowrap_implied": false,
// MANIFEST-NEXT: "null_noop": 0,
// MANIFEST-NEXT: "pointer_width": 32,
// MANIFEST-NEXT: "under": [
// MANIFEST-NEXT: "addr0-is-null"

!m = !sea.mem<!sea.ptr<64>, i8>
!m32 = !sea.mem<!sea.ptr<32>, i8>

// unsat, under a symbolic size placement assumes
sea.program @sym {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.nondet "n" [1] : i64
  %c1 = sea.const 1 : i64
  %ge = sea.icmp uge, %n, %c1 : i64
  sea.assume %ge
  %a = sea.malloc %n align 8 [2] : i64 -> !sea.ptr<64>
  %x = sea.load %m0[%a] : !m -> i8
  sea.halt
}

// unsat with no part: the check folds away, memhavoc-null-noop from the
// job alone
sea.program @mhnull {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %null = sea.ptr.null : !sea.ptr<64>
  %m2 = sea.memhavoc %m0[%null], %c16 [5] inplace : !m
  sea.halt
}

// sat: none
sea.program @oob {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [2] : i64
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  sea.halt
}

// unsat, an object of 2^31 units at 32 bits: no a-nowrap
sea.program @big {
  %m0 = sea.mem.nondet "m" [0] : !m32
  %sz = sea.const 2147483648 : i32
  %a = sea.malloc %sz align 8 [1] : i32 -> !sea.ptr<32>
  %n = sea.nondet "n" [2] : i32
  %c16 = sea.const 16 : i32
  %lt = sea.icmp ult, %n, %c16 : i32
  sea.assume %lt
  %p = sea.ptr.add %a, %n : !sea.ptr<32>, i32
  %x = sea.load %m0[%p] : !m32 -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
