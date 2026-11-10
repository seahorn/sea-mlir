!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %z = sea.const 0 : i8
  %one = sea.const 1 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %s = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %m1 = sea.memset %m0[%s], %one, %c16 inplace align 8 : !m
  %p = sea.load %m1[%s] align 8 : !m -> !sea.ptr<64>
  %n = sea.ptr.null : !sea.ptr<64>
  %ok = sea.ptr.cmp eq, %p, %n : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
