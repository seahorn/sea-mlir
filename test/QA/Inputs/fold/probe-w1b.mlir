sea.program @main {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<1>, i8>
  %k = sea.nondet "k" [1] : i1
  %a = sea.addr.from_int %k : i1 to !sea.addr<1>
  %v = sea.const 0x0201 : i16
  %m1 = sea.store %m[%a], %v inplace : !sea.mem<!sea.addr<1>, i8>, i16
  %x = sea.load %m1[%a] : !sea.mem<!sea.addr<1>, i8> -> i16
  %e = sea.icmp eq, %x, %v : i16
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
