!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %v = sea.const 9 : i8
  %b = sea.malloc %c4 align 4 [4] : i64 -> !sea.ptr<64>
  %c8 = sea.const 8 : i64
  %e = sea.ptr.add %b, %c8 : !sea.ptr<64>, i64
  %mu = sea.store %m0[%e], %v inplace : !m, i8
  %a = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %m2 = sea.memhavoc %mu[%a], %c4 [2] inplace : !m
  %x = sea.load %m2[%a] : !m -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
