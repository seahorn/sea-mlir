// RUN: sea-opt %s | sea-opt | FileCheck %s
// RUN: sea-opt %s --mlir-print-op-generic | sea-opt | FileCheck %s

// sea.ptr.index (docs/design/PtrIndex.md): pointer and address bases,
// narrow indices, negative strides and offset, each flag.
// CHECK-LABEL: sea.program @idx
sea.program @idx {
  %n = sea.const 64 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.const 3 : i64
  %j = sea.const -2 : i32
  // CHECK: sea.ptr.index %{{.*}}[%{{.*}}, %{{.*}}] strides [24, -4] offset 80 inbounds : !sea.ptr<64>, i64, i32
  %q = sea.ptr.index %p [%i, %j] strides [24, -4] offset 80 inbounds : !sea.ptr<64>, i64, i32
  // CHECK: sea.ptr.index %{{.*}}[%{{.*}}] strides [8] offset -16 : !sea.ptr<64>, i64
  %r = sea.ptr.index %q [%i] strides [8] offset -16 : !sea.ptr<64>, i64
  // CHECK: sea.ptr.index %{{.*}}[%{{.*}}] strides [1] offset 0 nusw nuw : !sea.ptr<64>, i64
  %s = sea.ptr.index %r [%i] strides [1] offset 0 nusw nuw : !sea.ptr<64>, i64
  %b = sea.const 100 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %k = sea.const 1 : i4
  // CHECK: sea.ptr.index %{{.*}}[%{{.*}}] strides [-128] offset 127 : !sea.addr<8>, i4
  %t = sea.ptr.index %a [%k] strides [-128] offset 127 : !sea.addr<8>, i4
  // a constant-only node with a promise (empty index list)
  // CHECK: sea.ptr.index %{{.*}}[] strides [] offset 8 inbounds : !sea.ptr<64>{{$}}
  %u = sea.ptr.index %p [] strides [] offset 8 inbounds : !sea.ptr<64>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
