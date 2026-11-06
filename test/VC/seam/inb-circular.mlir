// A-INB is assumed and reported, not discharged (docs/design/SeaM.md, "A-INB:
// assumed and reported"). The program writes 7 through a+n (n unconstrained:
// it can point into b), reads b, assumes the read is 7 and asserts it is 0.
// Plain: sat (n = b - a), replayed. Under A-INB the store is skipped when
// reading b, the read is 0 and the assume is false: --seam answers unsat and
// says `seam.inb: assumed`. That unsat holds for programs whose accesses stay
// in their objects; this one does not, and sea-vc does not tell (a
// bounds-instrumented memory-safety job would). The test pins the
// conditional nature of the answer.
// RUN: sea-vc %s --lower --solver=z3 | FileCheck %s --check-prefix=PLAIN
// RUN: %if yices %{ sea-vc %s --lower --solver=yices | FileCheck %s --check-prefix=PLAIN %}
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-inb=0 | FileCheck %s --check-prefix=NOINB
// RUN: sea-vc %s --lower --solver=z3 --seam 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: %if yices %{ sea-vc %s --lower --solver=yices --seam 2>&1 | FileCheck %s --check-prefix=SEAM %}
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-simplify=0 2>&1 | FileCheck %s --check-prefix=SEAM
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
  %hit = sea.icmp eq, %x, %v7 : i8
  sea.assume %hit
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
