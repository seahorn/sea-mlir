// PtrSort S2a: the deciders read nodes as they are (no old()). Two accesses
// at nodes over a loaded pointer p (no object, no view), p + 8i + 1 and
// p + 8i + 2, are decided apart by the OFFSET cancellation (cmp.cancel: the
// same base and index pairs, constants 1 and 2), with no assumption.
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower --seam --vc-typecheck 2>&1 | FileCheck %s --check-prefix=CHK-TC
// CHECK: seam.rule.cmp.cancel: {{[1-9]}}
// CHECK: result: unsat
// CHK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// CHK-TC-NOT: vc.ill-typed
// CHK-TC: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %cell = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %p = sea.load %m0[%cell] : !m -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %v = sea.nondet "v" [3] : i8
  %w = sea.nondet "w" [4] : i8
  %a1 = sea.ptr.index %p [%i] strides [8] offset 1 inbounds : !sea.ptr<64>, i64
  %a2 = sea.ptr.index %p [%i] strides [8] offset 2 inbounds : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%a1], %v inplace align 1 : !m, i8
  %m2 = sea.store %m1[%a2], %w inplace align 1 : !m, i8
  %r = sea.load %m2[%a1] align 1 : !m -> i8
  %ok = sea.icmp eq, %r, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
