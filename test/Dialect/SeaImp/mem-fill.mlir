// RUN: sea-opt %s | sea-opt | FileCheck %s
// RUN: sea-opt %s --mlir-print-op-generic | sea-opt | FileCheck %s

// seaimp.mem.fill and a memory variable with a constant start (init).

!m = !sea.mem<!sea.ptr<64>, i8>

// CHECK-LABEL: sea.program @fill
// CHECK: %[[M:.*]] = seaimp.var "mem" [0] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
// CHECK: %[[Z:.*]] = sea.const 0 : i8
// CHECK: seaimp.var "zero" init %[[Z]] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
// CHECK: seaimp.var "shadow" init %[[Z]] shadow : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
// CHECK: seaimp.mem.fill %[[M]][%{{.*}}], dense<[1, 2, 3]> : tensor<3xi8> : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
// CHECK: seaimp.mem.fill %[[M]][%{{.*}}], dense<7> : tensor<8xi8> tracked : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
sea.program @fill {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %z = sea.const 0 : i8
  %zero = seaimp.var "zero" init %z : !seaimp.var<!m>
  %sh = seaimp.var "shadow" init %z shadow : !seaimp.var<!m>
  %g = seaimp.global %mem @tab size 8 align 8 : !seaimp.var<!m> -> !sea.ptr<64>
  seaimp.mem.fill %mem[%g], dense<[1, 2, 3]> : tensor<3xi8> : !seaimp.var<!m>
  seaimp.mem.fill %mem[%g], dense<7> : tensor<8xi8> tracked : !seaimp.var<!m>
  %x = seaimp.load %zero[%g] : !seaimp.var<!m> -> i8
  %y = seaimp.load %sh[%g] : !seaimp.var<!m> -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
