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

// A memory variable (a region) and the region accesses.
// CHECK-LABEL: sea.program @region
sea.program @region {
  // CHECK: %[[M:.*]] = seaimp.var "mem" [0] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  %mem = seaimp.var "mem" [0] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  // CHECK: %[[H:.*]] = seaimp.var "heap" [1] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  %heap = seaimp.var "heap" [1] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  %p = sea.ptr.nondet "p" [2] : !sea.ptr<64>
  %q = sea.ptr.nondet "q" [3] : !sea.ptr<64>
  %v = sea.const 5 : i32
  %z = sea.const 0 : i8
  %n = sea.const 16 : i64
  // CHECK: seaimp.store %[[M]][%{{.*}}], %{{.*}} align 4 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>, i32
  seaimp.store %mem[%p], %v align 4 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>, i32
  // CHECK: seaimp.load %[[M]][%{{.*}}] align 4 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>> -> i32
  %x = seaimp.load %mem[%p] align 4 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>> -> i32
  // CHECK: seaimp.load %[[M]][%{{.*}}] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>> -> !sea.ptr<64>
  %y = seaimp.load %mem[%p] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>> -> !sea.ptr<64>
  // CHECK: seaimp.memset %[[M]][%{{.*}}], %{{.*}}, %{{.*}} align 8 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  seaimp.memset %mem[%p], %z, %n align 8 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  // CHECK: seaimp.memmove %[[M]][%{{.*}}], %[[M]][%{{.*}}], %{{.*}} align 4 src_align 2 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  seaimp.memmove %mem[%q], %mem[%p], %n align 4 src_align 2 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  // CHECK: seaimp.memmove %[[H]][%{{.*}}], %[[M]][%{{.*}}], %{{.*}} : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  seaimp.memmove %heap[%q], %mem[%p], %n : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  // CHECK: seaimp.memhavoc %[[M]][%{{.*}}], %{{.*}} [4] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  seaimp.memhavoc %mem[%p], %n [4] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  // CHECK: seaimp.get %[[M]] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  %m = seaimp.get %mem : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  // CHECK: seaimp.set %[[H]], %{{.*}} : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  seaimp.set %heap, %m : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
  sea.halt
}

// An address-indexed word memory: the index, length and unit types come
// from the region's type.
// CHECK-LABEL: sea.program @addr_region
sea.program @addr_region {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!sea.mem<!sea.addr<32>, i32, aligned>>
  %a = sea.const 8 : i32
  %w = sea.addr.from_int %a : i32 to !sea.addr<32>
  %z = sea.const 0 : i8
  %n = sea.const 8 : i32
  // CHECK: seaimp.memset %{{.*}}[%{{.*}}], %{{.*}}, %{{.*}} : !seaimp.var<!sea.mem<!sea.addr<32>, i32, aligned>>
  seaimp.memset %mem[%w], %z, %n : !seaimp.var<!sea.mem<!sea.addr<32>, i32, aligned>>
  // CHECK: seaimp.load %{{.*}}[%{{.*}}] : !seaimp.var<!sea.mem<!sea.addr<32>, i32, aligned>> -> i64
  %x = seaimp.load %mem[%w] : !seaimp.var<!sea.mem<!sea.addr<32>, i32, aligned>> -> i64
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
