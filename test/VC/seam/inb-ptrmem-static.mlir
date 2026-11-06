// inb-ptrmem.mlir with n = 48 (a numeral): the access through the reloaded
// pointer is a definite violation (offset 48 in a 16-byte object). That is
// the static fallback, a sanity check and not a soundness claim: --seam warns
// (vc.seam-inb-violated), answers without A-INB (`seam.inb: off`) and so gets
// the plain answer, sat with a replayed counterexample.
// RUN: sea-vc %s --lower --solver=z3 | FileCheck %s --check-prefix=PLAIN
// RUN: %if yices %{ sea-vc %s --lower --solver=yices | FileCheck %s --check-prefix=PLAIN %}
// RUN: sea-vc %s --lower --solver=z3 --seam 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: %if yices %{ sea-vc %s --lower --solver=yices --seam 2>&1 | FileCheck %s --check-prefix=SEAM %}
// PLAIN: result: sat
// PLAIN: replay: assert-failed
// SEAM: Warning: an access leaves its object{{.*}}[vc.seam-inb-violated]
// SEAM: seam.inb: off (fallback: an access leaves its object)
// SEAM: result: sat
// SEAM: replay: assert-failed
// SEAM: validate: checked={{[0-9]+}} mismatches=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ptrmem48 {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %c = sea.malloc %c16 align 16 [3] : i64 -> !sea.ptr<64>
  %d = sea.malloc %c16 align 16 [4] : i64 -> !sea.ptr<64>
  %n = sea.const 48 : i64
  %k = sea.nondet "k" [6] : i64
  %kok = sea.icmp ult, %k, %c16 : i64
  sea.assume %kok
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%c], %p inplace : !m, !sea.ptr<64>
  %bk = sea.ptr.add %b, %k : !sea.ptr<64>, i64
  %v5 = sea.const 5 : i8
  %m2 = sea.store %m1[%bk], %v5 inplace : !m, i8
  %q = sea.load %m2[%c] : !m -> !sea.ptr<64>
  %v9 = sea.const 9 : i8
  %m3 = sea.store %m2[%q], %v9 inplace : !m, i8
  %x = sea.load %m3[%d] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
