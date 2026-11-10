sea.program @main {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
  %v = sea.const 7 : i8
  %c0 = sea.const 4096 : i64
  %c1 = sea.const 4097 : i64
  %a = sea.addr.from_int %c0 : i64 to !sea.addr<64>
  %b = sea.addr.from_int %c1 : i64 to !sea.addr<64>
  %m1 = sea.store %m[%a], %v inplace : !sea.mem<!sea.addr<64>, i8>, i8
  %x = sea.load %m1[%b] : !sea.mem<!sea.addr<64>, i8> -> i8 loc("l.c":1:1)
  %c5 = sea.const 5 : i8
  %big = sea.icmp ugt, %x, %c5 : i8 loc("l.c":2:1)
  sea.assume %big loc("l.c":3:1)
  %f = sea.const 0 : i1
  sea.assert %f loc("l.c":4:1)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
