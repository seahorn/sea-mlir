// Ackermann lowering (docs/design/Ackermann.md) on a sat program: an initial
// memory read at symbolic, aliasing addresses (a + k, a + j, b + 2) and
// through a memmove and a memhavoc (reads of nd.<site> arrays only: no store
// in the query). With --ackermannize=on every solver's model is replayed
// through the rebuilt arrays: the run fails the assertion and every value
// equals the model's. %dead is never used: its cell is not read by the query
// (default 0, the same for replay and validation).
// RUN: sea-vc %s --lower --seam --ackermannize=on 2>&1 | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --seam --ackermannize=on --solver=yices 2>&1 | FileCheck %s %}
// RUN: sea-vc %s --lower --seam --ackermannize=off 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s --check-prefix=AUTO
// CHECK: vc.ackermann: on
// CHECK: vc.ackermann-constraints: {{[1-9][0-9]*}}
// CHECK: result: sat
// CHECK: vc.ackermann-model: defaulted={{[0-9]+}} inconsistent=0
// CHECK: replay: assert-failed
// CHECK: validate: checked={{[0-9]+}} mismatches=0
// OFF: vc.ackermann: off
// OFF: replay: assert-failed
// OFF: mismatches=0
// AUTO: vc.ackermann: on
// AUTO: mismatches=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @acksat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c32 = sea.const 32 : i64
  %a = sea.malloc %c32 align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.alloca %c32 align 8 [2] : i64 -> !sea.ptr<64>
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %c2 = sea.const 2 : i64
  %k = sea.nondet "k" [3] : i64
  %kle = sea.icmp ult, %k, %c16 : i64
  sea.assume %kle
  %j = sea.nondet "j" [4] : i64
  %jle = sea.icmp ult, %j, %c16 : i64
  sea.assume %jle
  %n = sea.nondet "n" [5] : i64
  %nle = sea.icmp ule, %n, %c8 : i64
  sea.assume %nle
  %pk = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %pj = sea.ptr.add %a, %j : !sea.ptr<64>, i64
  %pb2 = sea.ptr.add %b, %c2 : !sea.ptr<64>, i64
  %xk = sea.load %m0[%pk] : !m -> i8
  %xj = sea.load %m0[%pj] : !m -> i8
  %xb = sea.load %m0[%pb2] : !m -> i8
  %q = sea.nondet "q" [6] : i64
  %qle = sea.icmp ult, %q, %c32 : i64
  sea.assume %qle
  %pq = sea.ptr.add %b, %q : !sea.ptr<64>, i64
  %dead = sea.load %m0[%pq] : !m -> i8
  // b[0 .. n) <- a[0 .. n); b[16 .. 24) havocked
  %m1 = sea.memmove %m0[%b], %m0[%a], %n inplace : !m
  %pb16 = sea.ptr.add %b, %c16 : !sea.ptr<64>, i64
  %m2 = sea.memhavoc %m1[%pb16], %c8 [7] inplace : !m
  %pbk = sea.ptr.add %b, %k : !sea.ptr<64>, i64
  %yk = sea.load %m2[%pbk] : !m -> i8
  %c7 = sea.const 7 : i8
  %c9 = sea.const 9 : i8
  // fails e.g. with k = j, a[k] = 7, b[2] = 9, b[k] = 7 (k < n or k >= 16)
  %e1 = sea.icmp eq, %xk, %c7 : i8
  %e2 = sea.icmp eq, %xj, %c7 : i8
  %e3 = sea.icmp eq, %xb, %c9 : i8
  %e4 = sea.icmp eq, %yk, %c7 : i8
  %e12 = sea.and %e1, %e2 : i1
  %e34 = sea.and %e3, %e4 : i1
  %bad = sea.and %e12, %e34 : i1
  %t = sea.const 1 : i1
  %ok = sea.xor %bad, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
