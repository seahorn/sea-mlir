// Regression: sea-drop-fresh-memhavoc took a sea.mem.const root for
// unconstrained contents and dropped the memhavoc; the VC then pinned the
// object to the constant and answered unsat, while the program fails when
// the havoc gives another value.
// RUN: sea-vc %s --lower | FileCheck %s
// RUN: sea-run %s --input=2@heap.1+0=0 | FileCheck %s --check-prefix=INTERP
// CHECK: result: sat
// CHECK: replay: assert-failed
// INTERP: status: assert-failed
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %z = sea.const 9 : i8
  %m0 = sea.mem.const %z : !m
  %c4 = sea.const 4 : i64
  %p = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %m1 = sea.memhavoc %m0[%p], %c4 [2] inplace : !m
  %x = sea.load %m1[%p] : !m -> i8
  %k = sea.const 9 : i8
  %ok = sea.icmp eq, %x, %k : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
