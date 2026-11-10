!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c = sea.nondet "c" [5] : i1
  %c1 = sea.const 1 : i64
  %g = sea.malloc %c1 align 1 [6] : i64 -> !sea.ptr<64>
  %v = sea.const 1 : i8
  sea.cond_br %c, ^l, ^r
^l:
  %m1 = sea.store %m0[%g], %v inplace : !m, i8
  sea.br ^j(%m1 : !m)
^r:
  sea.br ^j(%m0 : !m)
^j(%mj: !m):
  %c4 = sea.const 4 : i64
  %p = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %m2 = sea.memhavoc %mj[%p], %c4 [2] inplace : !m
  %x = sea.load %m2[%p] : !m -> i8
  %k = sea.const 9 : i8
  %ok = sea.icmp ne, %x, %k : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
