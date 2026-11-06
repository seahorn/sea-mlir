// The load-dependent branch of inb-circular.mlir: the read of b decides
// whether the program can reach its end. Plain: n can point into b, the read
// is 7, the assertion (read == 0) fails: sat. Under A-INB the read is 0, the
// only way to the end is closed: --seam answers unsat, `seam.inb: assumed`
// (conditional on A-INB, which is not discharged).
// RUN: sea-vc %s --lower --solver=z3 | FileCheck %s --check-prefix=PLAIN
// RUN: %if yices %{ sea-vc %s --lower --solver=yices | FileCheck %s --check-prefix=PLAIN %}
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-inb=0 | FileCheck %s --check-prefix=NOINB
// RUN: sea-vc %s --lower --solver=z3 --seam 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: %if yices %{ sea-vc %s --lower --solver=yices --seam 2>&1 | FileCheck %s --check-prefix=SEAM %}
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
sea.program @branch {
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
  %hit = sea.icmp eq, %x, %v7 : i8
  sea.cond_br %hit, ^go, ^dead
^dead:
  sea.unreachable
^go:
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
