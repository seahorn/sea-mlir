// Null-object acceptance, row 2 (docs/design/SeaM-null.md): every
// alternative of the read address p = ite(c, null, A + 32) (A has 16 bytes)
// is impossible for an access of positive width, so the access is a definite
// violation (guarded aggregation: only when *every* alternative is): the
// static fallback warns vc.seam-inb-violated and runs without A-INB. The
// answer is the plain one, sat, replayed on the placed program. The safety
// job (and the interpreter) reports the violation: null or out of bounds.
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --seam --seam-check --solver=z3 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: %if yices %{ sea-vc %s --lower --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=SEAM %}
// RUN: not sea-run %s --input=3=1 --missing=zero | FileCheck %s --check-prefix=NULL
// RUN: not sea-run %s --input=3=0 --missing=zero | FileCheck %s --check-prefix=OOB
// PLAIN: result: sat
// PLAIN: replay: assert-failed
// SEAM: Warning: an access leaves its object{{.*}}[vc.seam-inb-violated]
// SEAM: seam.inb: off (fallback: an access leaves its object)
// SEAM: result: sat
// SEAM: replay: assert-failed
// SEAM: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// NULL: status: error
// NULL: message: memory access through the null pointer
// OOB: status: error
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @allinvalid {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [3] : i1
  %c32 = sea.const 32 : i64
  %A32 = sea.ptr.add %A, %c32 : !sea.ptr<64>, i64
  %null = sea.ptr.null : !sea.ptr<64>
  %p = sea.select %c, %null, %A32 : !sea.ptr<64>
  %x = sea.load %m0[%p] : !m -> i8
  %z = sea.const 0 : i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
