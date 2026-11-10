// a stored null read back as an integer, and ptr.to_int of null: 0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c8 = sea.const 8 : i64
  %s = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.ptr.null : !sea.ptr<64>
  %m1 = sea.store %m0[%s], %n inplace align 8 : !m, !sea.ptr<64>
  %x = sea.load %m1[%s] align 8 : !m -> i64
  %i = sea.ptr.to_int %n : !sea.ptr<64> to i64
  %zero = sea.const 0 : i64
  %a = sea.icmp eq, %x, %zero : i64
  %b = sea.icmp eq, %i, %zero : i64
  %ok = sea.and %a, %b : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
