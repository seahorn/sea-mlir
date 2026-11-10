sea.program @main {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<2>, i32, aligned>
  %k = sea.nondet "k" [1] : i2
  %a = sea.addr.from_int %k : i2 to !sea.addr<2>
  %v = sea.const 0x04030201 : i32
  %m1 = sea.store %m[%a], %v inplace : !sea.mem<!sea.addr<2>, i32, aligned>, i32
  %x = sea.load %m1[%a] : !sea.mem<!sea.addr<2>, i32, aligned> -> i32
  %e = sea.icmp eq, %x, %v : i32
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
