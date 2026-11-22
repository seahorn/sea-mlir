// RUN: sea-opt %s | sea-opt | FileCheck %s
// RUN: sea-opt %s --mlir-print-op-generic | sea-opt | FileCheck %s

// sea.mem.fill: constant cells of the memory's cell type from an address.

!m = !sea.mem<!sea.ptr<64>, i8>
!w = !sea.mem<!sea.addr<64>, i64, aligned>

// CHECK-LABEL: sea.program @fill
// CHECK: sea.mem.fill %{{.*}}[%{{.*}}], dense<[1, 2, 3]> : tensor<3xi8> inplace : !sea.mem<!sea.ptr<64>, i8>
// CHECK: sea.mem.fill %{{.*}}[%{{.*}}], dense<0> : tensor<4096xi8> inplace tracked : !sea.mem<!sea.ptr<64>, i8>
// CHECK: sea.mem.fill %{{.*}}[%{{.*}}], dense<[1, -1]> : tensor<2xi64> : !sea.mem<!sea.addr<64>, i64, aligned>
sea.program @fill {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %g = sea.global @tab size 8192 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3]> : tensor<3xi8> inplace : !m
  %m2 = sea.mem.fill %m1[%g], dense<0> : tensor<4096xi8> inplace tracked : !m
  %w0 = sea.mem.nondet "words" [1] : !w
  %z = sea.const 0 : i64
  %a = sea.addr.from_int %z : i64 to !sea.addr<64>
  %w1 = sea.mem.fill %w0[%a], dense<[1, -1]> : tensor<2xi64> : !w
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
