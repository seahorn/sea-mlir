// A pointer a+n is stored in c, a symbolic store into b (k assumed < 16: in
// bounds) follows, the pointer is reloaded from c and stored through. Plain:
// n = 48 makes a+n = d, x = 9, sat (replayed). Under A-INB the rewriting skips
// the store to b, the reload is a+n and the store through it is taken to stay
// in a: --seam answers unsat, `seam.inb: assumed`. The answer is conditional
// on A-INB, which sea-vc does not discharge (an earlier attempt that did, with
// a second encoding, gave wrong answers on exactly this shape: a pointer
// stored in memory and reloaded is unattributed in the encoding without
// A-INB). The test pins the conditional nature.
// RUN: sea-vc %s --lower --solver=z3 | FileCheck %s --check-prefix=PLAIN
// RUN: %if yices %{ sea-vc %s --lower --solver=yices | FileCheck %s --check-prefix=PLAIN %}
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-inb=0 | FileCheck %s --check-prefix=NOINB
// RUN: sea-vc %s --lower --solver=z3 --seam 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: %if yices %{ sea-vc %s --lower --solver=yices --seam 2>&1 | FileCheck %s --check-prefix=SEAM %}
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-arm=0 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-store-map=0 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --lower --solver=z3 --seam --widen-memory=0 2>&1 | FileCheck %s --check-prefix=SEAM
// The reload resolves to the stored pointer (ptr.load-resolve, PtrSort.md
// S1.6b: from_bits(to_bits(p)) ~> p), which keeps its view.
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=RESOLVE
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefix=RESOLVE
// RESOLVE: seam.rule.ptr.load-resolve: {{[1-9]}}
// RESOLVE: result: unsat
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
sea.program @ptrmem {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %c = sea.malloc %c16 align 16 [3] : i64 -> !sea.ptr<64>
  %d = sea.malloc %c16 align 16 [4] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [5] : i64
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
