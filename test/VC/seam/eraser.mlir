// PtrSort S2e (sat: the loaded pointer q may be A + i + 2): with --seam every address is ptr(w) and a memory indexed by
// addresses is array(ptr(w), bv(8)); the boundary erases the sort. The
// query has no ptr sort, node, marker or ptr-eq; arrays are bv-indexed;
// constants keep their names. --vc-typecheck (default on) passes on the
// sorted VC.
// RUN: sea-vc %s --lower --seam --ackermannize=off --smt2 2>/dev/null | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-wide --ackermannize=off --smt2 2>/dev/null | FileCheck %s
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s --check-prefix=RES
// RUN: sea-vc %s --lower --seam --seam-wide --seam-words=all --words-check 2>&1 | FileCheck %s --check-prefix=RES
// CHECK-NOT: ptr
// CHECK-NOT: addr
// CHECK: (Array (_ BitVec 64) (_ BitVec 8))
// CHECK-NOT: ptr
// CHECK: obj.heap.1
// CHECK-NOT: ptr
// CHECK: (check-sat)
// RES-NOT: vc.ill-typed
// RES-NOT: vc.ptr-sort-at-boundary
// RES: result: sat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %cell = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [3] : i64
  %v = sea.nondet "v" [4] : i8
  %p = sea.ptr.index %A [%i] strides [1] offset 2 inbounds : !sea.ptr<64>, i64
  %q = sea.load %m0[%cell] align 8 : !m -> !sea.ptr<64>
  %w = sea.load %m0[%p] align 1 : !m -> i8
  %m2 = sea.store %m0[%q], %v inplace align 1 : !m, i8
  %r = sea.load %m2[%p] align 1 : !m -> i8
  %ok = sea.icmp eq, %r, %w : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
