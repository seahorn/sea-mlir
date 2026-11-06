// (char*)((uintptr_t)a + ((uintptr_t)b - (uintptr_t)a)) is b: the access is in
// bounds of b. The rewriter must not read the sum as a pointer into a (it
// mentions b in its offset) and skip the earlier store to b. The plain VC is
// sat (the assertion fails).
// RUN: sea-vc %s --lower | FileCheck %s
// RUN: sea-vc %s --lower --seam | FileCheck %s
// CHECK: result: sat
// CHECK: replay: assert-failed
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @int {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %v7 = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %v7 inplace : !m, i8
  %ia = sea.ptr.to_int %a : !sea.ptr<64> to i64
  %ib = sea.ptr.to_int %b : !sea.ptr<64> to i64
  %d = sea.sub %ib, %ia : i64
  %ir = sea.add %ia, %d : i64
  %r = sea.ptr.from_int %ir : i64 to !sea.ptr<64>
  %x = sea.load %m1[%r] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
