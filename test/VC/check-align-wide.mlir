// An align larger than the address space (2^9 > 2^8): the promise holds only
// at address 0, so --check-align asserts (addr & 255) == 0. The segment is at
// 128: the promise is broken (sat). Debug builds used to abort on the mask.
// RUN: sea-vc %s --lower --check-align | FileCheck %s
// CHECK: result: sat
// CHECK: replay: assert-failed
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @alignbig {
  %a = sea.segment "heap.1" base 128 : i8 size 16 : i8 align 1 kind "heap" : !sea.addr<8>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %y = sea.load %m0[%a] align 512 : !am -> i8
  %ok = sea.icmp eq, %y, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
