// --ro-arrays=uf (docs/design/Ackermann.md, "Read-only arrays as functions")
// on widened memory (--widen-memory=4): the initial memory is an array of
// 4-byte words, so its function takes the array's own sorts, a 64-bit index
// and a 32-bit cell. Unsat only by functional consistency: the words at
// a + 4k and a + 4j are equal when k = j.
// RUN: sea-vc %s --lower --seam --widen-memory=4 --ackermannize=off --ro-arrays=uf --smt2 2>/dev/null | FileCheck %s --check-prefix=SMT
// RUN: sea-vc %s --lower --seam --widen-memory=4 --ackermannize=off --ro-arrays=uf 2>&1 | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --seam --widen-memory=4 --ackermannize=off --ro-arrays=uf --solver=yices 2>&1 | FileCheck %s %}
// RUN: %if bitwuzla %{ sea-vc %s --lower --seam --widen-memory=4 --ackermannize=off --ro-arrays=uf --solver=bitwuzla 2>&1 | FileCheck %s %}
// SMT: (declare-fun uf.{{[^ ]+}} ((_ BitVec 64)) (_ BitVec 32))
// SMT-NOT: Array
// CHECK: vc.ro-arrays: uf=1 kept-array=0
// CHECK: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ufwide {
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
  %eq = sea.icmp eq, %k, %j : i64
  %ne = sea.icmp ne, %xk, %xj : i32
  %bad = sea.and %eq, %ne : i1
  %t = sea.const 1 : i1
  %ok = sea.xor %bad, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
