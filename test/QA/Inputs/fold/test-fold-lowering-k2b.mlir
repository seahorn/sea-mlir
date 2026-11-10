sea.program @main {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<2>, i8, keyed>
  %k = sea.nondet "k" [1] : i2
  %a = sea.addr.from_int %k : i2 to !sea.addr<2>
  %v = sea.const 0x0504030201 : i40
  %m1 = sea.store %m[%a], %v inplace : !sea.mem<!sea.addr<2>, i8, keyed>, i40
  %x = sea.load %m1[%a] : !sea.mem<!sea.addr<2>, i8, keyed> -> i40
  %e = sea.icmp eq, %x, %v : i40
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
