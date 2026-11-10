!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c = sea.nondet "c" [3] : i1
  %c4 = sea.const 4 : i64
  %v = sea.const 9 : i8
  sea.cond_br %c, ^l, ^r
^l:
  %a = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  sea.br ^j(%a : !sea.ptr<64>)
^r:
  %n = sea.ptr.null : !sea.ptr<64>
  sea.br ^j(%n : !sea.ptr<64>)
^j(%p: !sea.ptr<64>):
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %m2 = sea.memhavoc %m1[%p], %c4 [2] inplace : !m
  %x = sea.load %m2[%p] : !m -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assume %c
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
