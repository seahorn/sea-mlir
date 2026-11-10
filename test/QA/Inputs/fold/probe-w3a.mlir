sea.program @main {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<3>, i8>
  %k = sea.nondet "k" [1] : i3
  %a = sea.addr.from_int %k : i3 to !sea.addr<3>
  %v = sea.const 0x0102030405060708090a0b0c0d0e0f10 : i128
  %m1 = sea.store %m[%a], %v inplace : !sea.mem<!sea.addr<3>, i8>, i128
  %x = sea.load %m1[%a] : !sea.mem<!sea.addr<3>, i8> -> i128
  %e = sea.icmp eq, %x, %v : i128
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
