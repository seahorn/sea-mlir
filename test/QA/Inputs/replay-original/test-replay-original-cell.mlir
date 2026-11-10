!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i64
  %p = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %c3 = sea.const 3 : i64
  %o = sea.and %i, %c3 : i64
  %q = sea.ptr.add %p, %o : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] : !m -> i8
  %k = sea.const 42 : i8
  %ok = sea.icmp ne, %x, %k : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
