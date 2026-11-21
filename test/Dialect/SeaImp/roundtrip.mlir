// RUN: sea-opt %s | sea-opt | FileCheck %s
// RUN: sea-opt %s --mlir-print-op-generic | sea-opt | FileCheck %s

// CHECK-LABEL: sea.program @vars
sea.program @vars {
  // CHECK: %[[X:.*]] = seaimp.var "x" [1] : !seaimp.var<i32>
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  // CHECK: %[[P:.*]] = seaimp.var "p" [2] : !seaimp.var<!sea.ptr<64>>
  %p = seaimp.var "p" [2] : !seaimp.var<!sea.ptr<64>>
  // CHECK: %[[S:.*]] = seaimp.var "s" [3] : !seaimp.var<!sea.struct<i8, !sea.ptr<64>>>
  %s = seaimp.var "s" [3] : !seaimp.var<!sea.struct<i8, !sea.ptr<64>>>
  %a = sea.nondet "nd" [4] : i32
  // CHECK: seaimp.set %[[X]], %{{.*}} : !seaimp.var<i32>
  seaimp.set %x, %a : !seaimp.var<i32>
  // CHECK: %{{.*}} = seaimp.get %[[X]] : !seaimp.var<i32>
  %v = seaimp.get %x : !seaimp.var<i32>
  // CHECK: seaimp.get %[[P]] : !seaimp.var<!sea.ptr<64>>
  %q = seaimp.get %p : !seaimp.var<!sea.ptr<64>>
  // CHECK: seaimp.get %[[S]] : !seaimp.var<!sea.struct<i8, !sea.ptr<64>>>
  %t = seaimp.get %s : !seaimp.var<!sea.struct<i8, !sea.ptr<64>>>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
