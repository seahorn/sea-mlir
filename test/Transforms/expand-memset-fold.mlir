// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-expand-memset{max-length=3}))' 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VC

// A length computed from constants (calloc's n * size, here 5 * 4 = 20) is
// a constant: folded, encoded as a store chain, never cut by the bound of
// symbolic lengths. (Treating it as symbolic once assumed 20 <= 16 and made
// SeaHorn's calloc.01 unsat.)
// CHECK-NOT: memset.symbolic-bound
// CHECK-NOT: sea.assume
// CHECK: sea.memset %{{.*}}, %{{.*}}, %[[C:.*]] inplace
// VC: result: unsat
sea.program @main {
  %m0 = sea.mem.nondet "r" [0] : !sea.mem<!sea.addr<8>, i8>
  %a = sea.const 5 : i8
  %b = sea.const 4 : i8
  %n = sea.mul %a, %b : i8
  %p = sea.const 16 : i8
  %pa = sea.addr.from_int %p : i8 to !sea.addr<8>
  %v = sea.const 0 : i8
  %m1 = sea.memset %m0[%pa], %v, %n inplace : !sea.mem<!sea.addr<8>, i8>
  %q = sea.const 35 : i8
  %qa = sea.addr.from_int %q : i8 to !sea.addr<8>
  %x = sea.load %m1[%qa] : !sea.mem<!sea.addr<8>, i8> -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
