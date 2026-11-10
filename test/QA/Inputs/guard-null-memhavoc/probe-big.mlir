!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %c8 = sea.const 8 : i64
  %p = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %m1 = sea.memhavoc %m0[%p], %c8 [2] inplace : !m
  %x = sea.load %m1[%p] : !m -> i8
  %k = sea.const 9 : i8
  %ok = sea.icmp ne, %x, %k : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
