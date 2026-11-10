!m = !sea.mem<!sea.addr<8>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %ai = sea.nondet "a" [1] : i8
  %a = sea.addr.from_int %ai : i8 to !sea.addr<8>
  %x = sea.load %m0[%a] : !m -> i8
  %k = sea.const 5 : i8
  %ok = sea.icmp ne, %x, %k : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
