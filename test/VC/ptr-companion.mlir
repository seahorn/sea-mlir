// PtrSort S6 (rev 5 2.4): the pointer companion. A pointer stored into a
// byte memory is split into bytes; reloaded, it is from_bits of a concat.
// --ptr-companion=exact reads the reload as ite(to_bits(c) = bits, c,
// from_bits(bits)), c the companion's cell at the load address, which holds
// the stored pointer (value-exact). Rev 4 6.2's acceptance cases:
//  @plain: stored and reloaded: c is &X and the guard folds (as the
//    bytes alone already do);
//  @overwritten: stored, then an integer store over its first byte,
//    reloaded: c is &X (the companion ignores integer stores) but the guard
//    does not fold (the load stays exact, unresolved); run with
//    --lower-opt=false: canonicalize would forward the integer byte to the
//    split reload's first byte, and the reload is no companion load;
//  @moved: stored, memmoved to another object, reloaded from the copy: the
//    companion is copied too (mcopy) and the guard folds.
// Verdicts are the same with and without the companion, and no load
// resolves that did not without it: exact, the companion only pays under
// A-PTRINT (rev 4 D11: "a pointer loaded from memory is the one last stored
// there"). --ptr-companion=assume drops the guard (seam.ptrint: assumed;
// @overwritten then answers unsat: the premise fails there, which
// --ptr-companion=check, the discharge, finds: the guards are the property).
// RUN: sea-vc %s --lower --lower-opt=false --program=overwritten --seam --ptr-companion=assume 2>&1 | FileCheck %s --check-prefix=ASSUME
// Under the default lowering the first byte of the reload is the integer
// just stored there (forwarded): no companion load, nothing rests on
// A-PTRINT, and the answer is the exact one.
// RUN: sea-vc %s --lower --program=overwritten --seam --ptr-companion=assume 2>&1 | FileCheck %s --check-prefix=FWD
// FWD: vc.ptr-companion: loads=0
// FWD-NOT: seam.ptrint: assumed
// FWD: result: sat
// FWD: replay: assert-failed
// RUN: sea-vc %s --lower --lower-opt=false --program=overwritten --seam --ptr-companion=check 2>&1 | FileCheck %s --check-prefix=VIOL
// RUN: sea-vc %s --lower --program=moved --seam --ptr-companion=check 2>&1 | FileCheck %s --check-prefix=HOLDS
// With --expand-memmove the memmove of @moved is kept (byte stores would
// drop the companion's copy of the stored pointer): the check still holds.
// RUN: sea-vc %s --lower --program=moved --seam --ptr-companion=check --expand-memmove=8 2>&1 | FileCheck %s --check-prefix=HOLDS,KEEP
// KEEP: vc.expand-memmove: k=8 expanded=0 {{.*}} kept-ptr-companion=1
// KEEP: vc.ptr-companion: loads=1 stores=1 copies=1
// ASSUME: seam.ptrint: assumed loads=1
// ASSUME: result: unsat
// VIOL: seam.ptrint: checked loads=1
// VIOL: result: sat
// VIOL: seam.ptrint: not discharged
// VIOL: replay: assert-failed
// VIOL: seam.ptrint-violated: {{.*}} stored={{[0-9]+}}
// VIOL: seam.ptrint-replay: loads=1 holds=0 violated=1 unstored=0
// HOLDS: seam.ptrint: checked loads=1
// HOLDS: result: unsat
// HOLDS: seam.ptrint: holds
// RUN: sea-vc %s --lower --program=plain --seam --ptr-companion=exact 2>&1 | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --lower-opt=false --program=overwritten --seam --ptr-companion=exact 2>&1 | FileCheck %s --check-prefix=OVER
// RUN: sea-vc %s --lower --program=moved --seam --ptr-companion=exact 2>&1 | FileCheck %s --check-prefix=MOVED
// RUN: sea-vc %s --lower --program=moved --seam 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: sea-vc %s --lower --program=moved --seam --ptr-companion=exact --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --lower-opt=false --program=overwritten --seam --ptr-companion=exact --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: not sea-vc %s --lower --program=plain --ptr-companion=exact 2>&1 | FileCheck %s --check-prefix=NEED
// PLAIN: vc.ptr-companion: loads=1 stores=1 copies=0
// PLAIN: vc.ptr-companion-arrays: 0
// PLAIN: result: unsat
// OVER: vc.ptr-companion: loads=1 stores=1 copies=0
// OVER: vc.ptr-companion-arrays: 0
// OVER: result: sat
// OVER: validate: checked={{[0-9]+}} mismatches=0
// MOVED: vc.ptr-companion: loads=1 stores=1 copies=1
// MOVED: vc.ptr-companion-arrays: 0
// MOVED: result: unsat
// OFF-NOT: vc.ptr-companion
// OFF: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// NEED: --ptr-companion{{.*}}need{{.*}}--seam
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @plain {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %X = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [3] : !m
  %m1 = sea.store %m0[%A], %X inplace : !m, !sea.ptr<64>
  %q = sea.load %m1[%A] : !m -> !sea.ptr<64>
  %eq = sea.ptr.cmp eq, %q, %X : !sea.ptr<64>
  sea.assert %eq
  sea.halt
}
sea.program @overwritten {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %X = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [3] : !m
  %b = sea.nondet "b" [4] : i8
  %m1 = sea.store %m0[%A], %X inplace : !m, !sea.ptr<64>
  %m2 = sea.store %m1[%A], %b inplace : !m, i8
  %q = sea.load %m2[%A] : !m -> !sea.ptr<64>
  %eq = sea.ptr.cmp eq, %q, %X : !sea.ptr<64>
  sea.assert %eq
  sea.halt
}
sea.program @moved {
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %X = sea.malloc %c8 align 8 [3] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %m1 = sea.store %m0[%A], %X inplace : !m, !sea.ptr<64>
  %m2 = sea.memmove %m1[%B], %m1[%A], %c8 inplace : !m
  %q = sea.load %m2[%B] : !m -> !sea.ptr<64>
  %eq = sea.ptr.cmp eq, %q, %X : !sea.ptr<64>
  sea.assert %eq
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
