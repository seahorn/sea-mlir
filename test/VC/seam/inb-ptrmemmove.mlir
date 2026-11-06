// inb-ptrmem.mlir with the pointer moved by a memmove (c to e) before the
// reload. Same expectation: plain / --seam-inb=0 sat (n = 48: a+n = d,
// replayed); --seam unsat with `seam.inb: assumed` (conditional on A-INB).
// RUN: sea-vc %s --lower --solver=z3 | FileCheck %s --check-prefix=PLAIN
// RUN: %if yices %{ sea-vc %s --lower --solver=yices | FileCheck %s --check-prefix=PLAIN %}
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-inb=0 | FileCheck %s --check-prefix=NOINB
// RUN: sea-vc %s --lower --solver=z3 --seam 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: %if yices %{ sea-vc %s --lower --solver=yices --seam 2>&1 | FileCheck %s --check-prefix=SEAM %}
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-arm=0 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-store-map=0 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --lower --solver=z3 --seam --widen-memory=0 2>&1 | FileCheck %s --check-prefix=SEAM
// PLAIN: result: sat
// PLAIN: replay: assert-failed
// NOINB: seam.inb: off
// NOINB: result: sat
// NOINB: replay: assert-failed
// NOINB: validate: checked={{[0-9]+}} mismatches=0
// SEAM-NOT: vc.seam-inb-violated
// SEAM: seam.inb: assumed accesses={{[0-9]+}} conditions={{[1-9][0-9]*}}
// SEAM: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ptrmemmove {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %c = sea.malloc %c16 align 16 [3] : i64 -> !sea.ptr<64>
  %d = sea.malloc %c16 align 16 [4] : i64 -> !sea.ptr<64>
  %e = sea.malloc %c16 align 16 [7] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [5] : i64
  %k = sea.nondet "k" [6] : i64
  %kok = sea.icmp ult, %k, %c16 : i64
  sea.assume %kok
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%c], %p inplace : !m, !sea.ptr<64>
  %bk = sea.ptr.add %b, %k : !sea.ptr<64>, i64
  %v5 = sea.const 5 : i8
  %m2 = sea.store %m1[%bk], %v5 inplace : !m, i8
  %m3 = sea.memmove %m2[%e], %m2[%c], %c8 inplace : !m
  %q = sea.load %m3[%e] : !m -> !sea.ptr<64>
  %v9 = sea.const 9 : i8
  %m4 = sea.store %m3[%q], %v9 inplace : !m, i8
  %x = sea.load %m4[%d] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
