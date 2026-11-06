!m = !sea.mem<!sea.addr<4>, i8>
sea.program @oversized {
  %zero = sea.const 0 : i8
  %m0 = sea.mem.const %zero : !m
  %ai = sea.const 0 : i4
  %a = sea.addr.from_int %ai : i4 to !sea.addr<4>
  %v = sea.const 1 : i136
  %m1 = sea.store %m0[%a], %v inplace : !m, i136
  %x = sea.load %m1[%a] : !m -> i16
  %expect = sea.const 0 : i16
  %ok = sea.icmp eq, %x, %expect : i16
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
