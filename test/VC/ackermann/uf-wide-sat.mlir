// --ro-arrays=uf on widened memory, a sat program: the model's function
// values (32-bit words at symbolic indices) rebuild the array for replay and
// validation on every backend.
// RUN: sea-vc %s --lower --seam --widen-memory=4 --ackermannize=off --ro-arrays=uf 2>&1 | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --seam --widen-memory=4 --ackermannize=off --ro-arrays=uf --solver=yices 2>&1 | FileCheck %s %}
// RUN: %if bitwuzla %{ sea-vc %s --lower --seam --widen-memory=4 --ackermannize=off --ro-arrays=uf --solver=bitwuzla 2>&1 | FileCheck %s %}
// CHECK: vc.ro-arrays: uf=1 kept-array=0
// CHECK: result: sat
// CHECK: vc.ackermann-model: defaulted={{[0-9]+}} inconsistent=0
// CHECK: replay: assert-failed
// CHECK: validate: checked={{[0-9]+}} mismatches=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ufwidesat {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64 = sea.const 64 : i64
  %a = sea.malloc %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %c4 = sea.const 4 : i64
  %c16 = sea.const 16 : i64
  %k = sea.nondet "k" [2] : i64
  %kle = sea.icmp ult, %k, %c16 : i64
  sea.assume %kle
  %j = sea.nondet "j" [3] : i64
  %jle = sea.icmp ult, %j, %c16 : i64
  sea.assume %jle
  %ok4 = sea.mul %k, %c4 : i64
  %oj4 = sea.mul %j, %c4 : i64
  %pk = sea.ptr.add %a, %ok4 : !sea.ptr<64>, i64
  %pj = sea.ptr.add %a, %oj4 : !sea.ptr<64>, i64
  %xk = sea.load %m0[%pk] align 4 : !m -> i32
  %xj = sea.load %m0[%pj] align 4 : !m -> i32
  // fails with k != j, a[k] = 70000, a[j] = 90000
  %c7 = sea.const 70000 : i32
  %c9 = sea.const 90000 : i32
  %e1 = sea.icmp eq, %xk, %c7 : i32
  %e2 = sea.icmp eq, %xj, %c9 : i32
  %bad = sea.and %e1, %e2 : i1
  %t = sea.const 1 : i1
  %ok = sea.xor %bad, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
