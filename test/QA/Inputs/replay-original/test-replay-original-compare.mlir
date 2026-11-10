!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ope {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %c = sea.malloc %c16 align 16 [3] : i64 -> !sea.ptr<64>
  %v7 = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %v7 inplace : !m, i8
  %e = sea.ptr.add %a, %c16 : !sea.ptr<64>, i64
  %eq = sea.ptr.cmp eq, %e, %b : !sea.ptr<64>
  %s = sea.select %eq, %b, %c : !sea.ptr<64>
  %x = sea.load %m1[%s] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
