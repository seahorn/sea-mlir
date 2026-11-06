// Ackermann lowering (docs/design/Ackermann.md) on an unsat program whose
// proof needs the functional-consistency constraints: two reads of the
// initial memory at a + k and a + j with k = j, a read of the copy b[k] = a[k]
// (k < n; its index a + ((b + k) - b) has the linear form of a + k: one
// constant), and a havocked cell read twice through different address terms.
// RUN: sea-vc %s --lower --seam --ackermannize=on 2>&1 | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --seam --ackermannize=on --solver=yices 2>&1 | FileCheck %s %}
// RUN: sea-vc %s --lower --seam --ackermannize=off 2>&1 | FileCheck %s --check-prefix=OFF
// CHECK: vc.ackermann: on
// CHECK: vc.ackermann-constraints: {{[1-9][0-9]*}}
// CHECK: result: unsat
// OFF: vc.ackermann: off
// OFF: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ackunsat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32 = sea.const 32 : i64
  %a = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.alloca %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %k = sea.nondet "k" [3] : i64
  %kle = sea.icmp ult, %k, %c8 : i64
  sea.assume %kle
  %j = sea.nondet "j" [4] : i64
  %jk = sea.icmp eq, %j, %k : i64
  sea.assume %jk
  %n = sea.nondet "n" [5] : i64
  %nle = sea.icmp ule, %n, %c16 : i64
  sea.assume %nle
  %kn = sea.icmp ult, %k, %n : i64
  sea.assume %kn
  %pk = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %pj = sea.ptr.add %a, %j : !sea.ptr<64>, i64
  %xk = sea.load %m0[%pk] : !m -> i8
  %xj = sea.load %m0[%pj] : !m -> i8
  %m1 = sea.memmove %m0[%b], %m0[%a], %n inplace : !m
  %pb16 = sea.ptr.add %b, %c16 : !sea.ptr<64>, i64
  %m2 = sea.memhavoc %m1[%pb16], %c8 [7] inplace : !m
  %pbk = sea.ptr.add %b, %k : !sea.ptr<64>, i64
  %yk = sea.load %m2[%pbk] : !m -> i8
  // the havocked cell b + 16 + j read as (b + 16) + j and (b + j) + 16
  %h1p = sea.ptr.add %pb16, %j : !sea.ptr<64>, i64
  %bj = sea.ptr.add %b, %j : !sea.ptr<64>, i64
  %h2p = sea.ptr.add %bj, %c16 : !sea.ptr<64>, i64
  %h1 = sea.load %m2[%h1p] : !m -> i8
  %h2 = sea.load %m2[%h2p] : !m -> i8
  %e1 = sea.icmp eq, %xk, %xj : i8
  %e2 = sea.icmp eq, %yk, %xk : i8
  %e3 = sea.icmp eq, %h1, %h2 : i8
  %e12 = sea.and %e1, %e2 : i1
  %e = sea.and %e12, %e3 : i1
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
