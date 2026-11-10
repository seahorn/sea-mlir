!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c4 = sea.const 4 : i64
  %a = sea.malloc %c4 align 1 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c4 align 1 [2] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [3] : i64 loc("o.c":1:1)
  %ge = sea.icmp uge, %i, %c4 : i64
  sea.assume %ge loc("o.c":2:1)
  %v = sea.const 7 : i8
  %y0 = sea.load %m0[%b] : !m -> i8
  %e0 = sea.icmp eq, %y0, %z : i8
  sea.assume %e0 loc("o.c":3:1)
  %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %y = sea.load %m1[%b] : !m -> i8
  %e = sea.icmp eq, %y, %v : i8
  sea.assume %e loc("o.c":5:1)
  %f = sea.const 0 : i1
  sea.assume %f loc("o.c":3:7)
  %t = sea.const 1 : i1
  sea.assert %t loc("o.c":4:1)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
