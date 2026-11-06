// An object ending exactly at 2^w (heap.1 = [246, 256) at 8-bit addresses)
// does not wrap: accepted (segments-wrap.mlir: base + size > 2^w is not).
// RUN: sea-vc %s --seam 2>&1 | FileCheck %s
// RUN: sea-vc %s 2>&1 | FileCheck %s
// CHECK: result: sat
// CHECK: replay: assert-failed
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @top {
  %a = sea.segment "heap.1" base 246 : i8 size 10 : i8 align 1 kind "heap" : !sea.addr<8>
  %b = sea.segment "heap.2" base 1 : i8 size 2 : i8 align 1 kind "heap" : !sea.addr<8>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %v7 = sea.const 7 : i8
  %c9 = sea.const 9 : i8
  %p = sea.addr.add %a, %c9 : !sea.addr<8>, i8
  %m1 = sea.store %m0[%p], %v7 inplace : !am, i8
  %x = sea.load %m1[%p] : !am -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
