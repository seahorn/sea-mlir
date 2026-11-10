!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %p = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %lo = sea.icmp uge, %i, %c4 : i64
  sea.assume %lo
  %c8 = sea.const 8 : i64
  %hi = sea.icmp ult, %i, %c8 : i64
  sea.assume %hi
  %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] : !m -> i8
  %k = sea.const 3 : i8
  %ok = sea.icmp ne, %x, %k : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
