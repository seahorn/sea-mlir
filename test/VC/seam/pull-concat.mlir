// PtrSort S6: a split pointer load (a concat of byte
// loads, no widening) whose bytes share an ite condition. One branch stores
// a pointer to X in slot T, the other an integer; after the join every byte
// of the reload is ite(c, byte_k(&X), byte_k(n)). --seam-pull-concat pulls
// the shared condition out of the concat and the then side reassembles &X:
// the load is ite(c, X, from_bits(n)) (ptr.load-resolve-concat,
// value-exact). Off by default; the verdict does not change.
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,OFF
// RUN: sea-vc %s --lower --seam --seam-stats --seam-pull-concat=4 2>&1 | FileCheck %s --check-prefixes=CHECK,ON
// RUN: sea-vc %s --lower --seam --seam-pull-concat=4 --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: not sea-vc %s --lower --seam-pull-concat=4 2>&1 | FileCheck %s --check-prefix=NEED
// OFF: seam.ptr.from-bits-left.concat: 1
// OFF-NOT: ptr.load-resolve-concat
// ON-NOT: seam.ptr.from-bits-left.concat
// ON: seam.rule.ptr.load-resolve-concat: 1
// CHECK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// NEED: --seam-pull-concat{{.*}}need{{.*}}--seam
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @join {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c8 = sea.const 8 : i64
  %T = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %X = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [3] : i64
  %c = sea.nondet "c" [4] : i1
  sea.cond_br %c, ^t, ^e
^t:
  %mt = sea.store %m0[%T], %X inplace : !m, !sea.ptr<64>
  sea.br ^j(%mt : !m)
^e:
  %me = sea.store %m0[%T], %n inplace : !m, i64
  sea.br ^j(%me : !m)
^j(%mj : !m):
  %q = sea.load %mj[%T] : !m -> !sea.ptr<64>
  %eq = sea.ptr.cmp eq, %q, %X : !sea.ptr<64>
  %t1 = sea.const 1 : i1
  %nc = sea.xor %c, %t1 : i1
  %ok = sea.or %nc, %eq : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
