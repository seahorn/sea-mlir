!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %c1 = sea.const 1 : i64
  %one = sea.const 1 : i8
  %a = sea.malloc %c4 align 1 [3] : i64 -> !sea.ptr<64>
  %n = sea.ptr.null : !sea.ptr<64>
  %a1 = sea.ptr.add %a, %c1 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%a1], %one inplace : !m, i8
  %m2 = sea.memhavoc %m1[%n], %c4 [2] inplace : !m
  %x = sea.load %m2[%a1] : !m -> i8
  %ok = sea.icmp eq, %x, %one : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
