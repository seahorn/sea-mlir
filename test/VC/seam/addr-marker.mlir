// The ADDR marker (docs/design/PtrSort.md 1.4, S1.6a): an ordered pointer
// compare observes the addresses, ADDR(p) <u ADDR(q) under --seam; an
// identity, erased at the boundary (a marker at the solver is the
// vc.ptr-sort-at-boundary error). The verdict is the same with and
// without SeaM. A + i <u A + 16 for i <u 16 (A placed: no wrap): unsat.
// RUN: sea-vc %s --lower | FileCheck %s
// RUN: sea-vc %s --lower --seam | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --seam --seam-wide --solver=yices | FileCheck %s %}
// CHECK-NOT: error
// CHECK: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ordered {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %lt = sea.icmp ult, %i, %c16 : i64
  sea.assume %lt
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %e = sea.ptr.add %A, %c16 : !sea.ptr<64>, i64
  %ok = sea.ptr.cmp ult, %p, %e : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
