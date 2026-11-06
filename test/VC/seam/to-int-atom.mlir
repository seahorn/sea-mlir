// PtrSort S2c: a node is a node at every position. In to_int(A + 4i) + 8
// the marker over the node is an atom of the integer sum (counted
// ptr.to-int-atom), not flattened into A + 4i + 8. The compare with
// to_int(A) + 4i + 8 stays valid (unsat), with and without SeaM.
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// RUN: sea-vc %s --lower 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK: seam.rule.ptr.to-int-atom: {{[1-9]}}
// CHECK: result: unsat
// CHK: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64 = sea.const 64 : i64
  %c8 = sea.const 8 : i64
  %c4 = sea.const 4 : i64
  %A = sea.malloc %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %v = sea.nondet "v" [3] : i8
  %p = sea.ptr.index %A [%i] strides [4] offset 0 inbounds : !sea.ptr<64>, i64
  %ip = sea.ptr.to_int %p : !sea.ptr<64> to i64
  %k = sea.add %ip, %c8 : i64
  %ia = sea.ptr.to_int %A : !sea.ptr<64> to i64
  %i4 = sea.mul %i, %c4 : i64
  %t = sea.add %ia, %i4 : i64
  %k2 = sea.add %t, %c8 : i64
  %same = sea.icmp eq, %k, %k2 : i64
  sea.assert %same
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
