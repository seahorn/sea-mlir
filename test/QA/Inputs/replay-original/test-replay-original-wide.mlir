!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c8 = sea.const 8 : i64
  %c4 = sea.const 4 : i64
  %p = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %q = sea.ptr.add %p, %c4 : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] align 4 : !m -> i32
  %k = sea.const 0x01020304 : i32
  %ok = sea.icmp ne, %x, %k : i32
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
