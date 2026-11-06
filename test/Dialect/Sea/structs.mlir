// RUN: sea-opt %s | sea-opt | FileCheck %s
// RUN: sea-opt %s --mlir-print-op-generic | sea-opt | FileCheck %s

// CHECK-LABEL: sea.program @structs
sea.program @structs {
  %a = sea.nondet "nondet_u16" [0] : i16
  %b = sea.nondet "nondet_u32" [1] : i32
  %c = sea.nondet "nondet_bool" [2] : i1
  // CHECK: sea.struct.create %{{.*}}, %{{.*}} : i16, i32 -> !sea.struct<i16, i32>
  %s = sea.struct.create %a, %b : i16, i32 -> !sea.struct<i16, i32>
  // CHECK: sea.struct.create %{{.*}}, %{{.*}} : !sea.struct<i16, i32>, i1 -> !sea.struct<!sea.struct<i16, i32>, i1>
  %n = sea.struct.create %s, %c : !sea.struct<i16, i32>, i1 -> !sea.struct<!sea.struct<i16, i32>, i1>
  // CHECK: sea.struct.extract %{{.*}}[1] : !sea.struct<i16, i32> -> i32
  %x = sea.struct.extract %s[1] : !sea.struct<i16, i32> -> i32
  // CHECK: sea.struct.insert %{{.*}}, %{{.*}}[0] : i16 into !sea.struct<i16, i32>
  %t = sea.struct.insert %a, %s[0] : i16 into !sea.struct<i16, i32>
  // CHECK: sea.select %{{.*}}, %{{.*}}, %{{.*}} : !sea.struct<i16, i32>
  %u = sea.select %c, %s, %t : !sea.struct<i16, i32>
  sea.halt
}

// CHECK-LABEL: sea.program @nondet
sea.program @nondet {
  // CHECK: sea.struct.nondet "undef" [0] : !sea.struct<i16, !sea.ptr<32>>
  %s = sea.struct.nondet "undef" [0] : !sea.struct<i16, !sea.ptr<32>>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
