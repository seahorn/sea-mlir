!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %one = sea.const 1 : i8
  %a = sea.malloc %c4 align 4 [3] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [1] : i1
  %n = sea.ptr.null : !sea.ptr<64>
  %p = sea.select %c, %a, %n : !sea.ptr<64>
  %m1 = sea.store %m0[%a], %one inplace : !m, i8
  %m2 = sea.memhavoc %m1[%p], %c4 [2] inplace : !m
  %x = sea.load %m2[%a] : !m -> i8
  %ok = sea.icmp eq, %x, %one : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
