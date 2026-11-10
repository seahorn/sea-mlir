!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %e = sea.ptr.add %a, %c16 : !sea.ptr<64>, i64
  %eq = sea.ptr.cmp eq, %e, %b : !sea.ptr<64>
  sea.assert %eq
  %t = sea.const 1 : i1
  %ne = sea.xor %eq, %t : i1
  sea.assert %ne
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
