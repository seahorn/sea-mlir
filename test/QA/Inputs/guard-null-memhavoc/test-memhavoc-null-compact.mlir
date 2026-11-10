!m = !sea.mem<!sea.ptr<16>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c4 = sea.const 4 : i16
  %c1 = sea.const 1 : i16
  %one = sea.const 1 : i8
  %b = sea.malloc %c4 align 1 [3] : i16 -> !sea.ptr<16>
  %a = sea.malloc %c4 align 1 [4] : i16 -> !sea.ptr<16>
  %n = sea.ptr.null : !sea.ptr<16>
  %b1 = sea.ptr.add %b, %c1 : !sea.ptr<16>, i16
  %m1 = sea.store %m0[%b1], %one inplace : !m, i8
  %m1a = sea.store %m1[%a], %one inplace : !m, i8
  %c = sea.nondet "c" [1] : i1
  %p = sea.select %c, %a, %n : !sea.ptr<16>
  %m2 = sea.memhavoc %m1a[%p], %c4 [2] inplace : !m
  %x = sea.load %m2[%b1] : !m -> i8
  %ok = sea.icmp eq, %x, %one : i8
  %t = sea.const 1 : i1
  %nc = sea.xor %c, %t : i1
  sea.assume %nc
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
