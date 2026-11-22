// A memmove into a buffer B whose destination is pointer arithmetic on B
// with an offset that mentions B's address: d = B + (to_int(B + 32) -
// to_int(q)), q = ite(c, B, B + k) (a pointer joined across paths, the
// ring buffer of an h2 flow-control job). d is a node over B, so it is a
// pointer into B whatever its offset mentions (view.node; SeaM.md
// "Soundness stance"), and the range test of a read of another object A
// is decided by A-SEP (row.mcopy-skip, cmp.diff-base). Before, the offset
// mentioned B, d had no view and ARM no entry: the range test stayed open.
// k, n <= 32 keep the move in B, j < 64 the read in A (A-INB holds:
// the plain VC is unsat too).
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// The bounds job checks the move against B (the offset companion of a
// ptr.add is its operand's plus the offset), so it discharges A-SEP here.
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off --timeout=60 2>&1 | FileCheck %s --check-prefix=INB
// CHECK: seam.rule.cmp.diff-base: {{[1-9]}}
// CHECK-NOT: seam.rule.row.mcopy:
// CHECK: seam.rule.row.mcopy-skip: 1
// CHECK: seam.rule.view.node: {{[1-9]}}
// CHECK: result: unsat
// PLAIN: result: unsat
// INB: check-inb.discharges: a-inb a-sep
// INB: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ring {
  %m0 = sea.mem.nondet "m" [9] : !m
  %c64 = sea.const 64 : i64
  %A = sea.malloc %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c64 align 8 [2] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c64 align 8 [3] : i64 -> !sea.ptr<64>
  %k = sea.nondet "k" [4] : i64
  %n = sea.nondet "n" [5] : i64
  %c = sea.nondet "c" [6] : i1
  %j = sea.nondet "j" [7] : i64
  %c32 = sea.const 32 : i64
  %kok = sea.icmp ule, %k, %c32 : i64
  sea.assume %kok
  %nok = sea.icmp ule, %n, %c32 : i64
  sea.assume %nok
  %jok = sea.icmp ult, %j, %c64 : i64
  sea.assume %jok
  %Bk = sea.ptr.add %B, %k : !sea.ptr<64>, i64
  %q = sea.select %c, %B, %Bk : !sea.ptr<64>
  %e = sea.ptr.add %B, %c32 : !sea.ptr<64>, i64
  %ie = sea.ptr.to_int %e : !sea.ptr<64> to i64
  %iq = sea.ptr.to_int %q : !sea.ptr<64> to i64
  %u = sea.sub %ie, %iq : i64
  %d = sea.ptr.add %B, %u : !sea.ptr<64>, i64
  %Aj = sea.ptr.add %A, %j : !sea.ptr<64>, i64
  %r = sea.load %m0[%Aj] : !m -> i8
  %m1 = sea.memmove %m0[%d], %m0[%C], %n inplace : !m
  %l = sea.load %m1[%Aj] : !m -> i8
  %ok = sea.icmp eq, %l, %r : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
