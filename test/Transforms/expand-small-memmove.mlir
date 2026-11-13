// RUN: sea-opt %s --split-input-file --sea-place-static --sea-expand-small-memmove=max-bytes=4 2>&1 | FileCheck %s
// RUN: sea-opt %s --split-input-file --sea-place-static --sea-expand-small-memmove=max-bytes=3 2>&1 | FileCheck %s --check-prefix=K3
// RUN: sea-opt %s --split-input-file --sea-expand-small-memmove=max-bytes=4 2>&1 | FileCheck %s --check-prefix=ABS

// sea-expand-small-memmove: a memmove of a constant length <= max-bytes
// becomes loads of every source byte, then the stores in address order;
// the align promises are kept only where they hold (the largest power of 2
// dividing the promise and the offset). Before placement (ABS) a memmove is
// kept: the abstract semantics has no bytes of a stored pointer.
// ABS-LABEL: sea.program @shape
// ABS-SAME: "kept-abstract" = 1
// ABS: sea.memmove

!m = !sea.mem<!sea.ptr<64>, i8>
// CHECK-LABEL: sea.program @shape
// CHECK-NOT: sea.memmove
// CHECK: %[[V0:.*]] = sea.load %[[SRC:[0-9a-z_]+]][%{{[0-9a-z_]+}}] align 8 :
// CHECK: %[[V1:.*]] = sea.load %[[SRC]][%{{[0-9a-z_]+}}] : !sea.mem
// CHECK: %[[V2:.*]] = sea.load %[[SRC]][%{{[0-9a-z_]+}}] align 2 :
// CHECK: %[[V3:.*]] = sea.load %[[SRC]][%{{[0-9a-z_]+}}] : !sea.mem
// CHECK: %[[M1:.*]] = sea.store %[[M0:[0-9a-z_]+]][%{{[0-9a-z_]+}}], %[[V0]] inplace align 4 :
// CHECK: %[[M2:.*]] = sea.store %[[M1]][%{{[0-9a-z_]+}}], %[[V1]] inplace :
// CHECK: %[[M3:.*]] = sea.store %[[M2]][%{{[0-9a-z_]+}}], %[[V2]] inplace align 2 :
// CHECK: %[[M4:.*]] = sea.store %[[M3]][%{{[0-9a-z_]+}}], %[[V3]] inplace :
// CHECK: sea.load %[[M4]]
// K3-LABEL: sea.program @shape
// K3: sea.memmove
sea.program @shape {
  %m0 = sea.mem.nondet "m" [1] : !m
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [2] : i64 -> !sea.ptr<64>
  %eight = sea.const 8 : i64
  %d = sea.ptr.add %p, %eight : !sea.ptr<64>, i64
  %four = sea.const 4 : i64
  %m1 = sea.memmove %m0[%d], %m0[%p], %four inplace align 4 src_align 8 : !m
  %x = sea.load %m1[%d] : !m -> i8
  %z = sea.const 0 : i8
  %ok = sea.icmp ne, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// -----

// Kept: a length over K, a symbolic length; a length 0 is its input.
!m = !sea.mem<!sea.ptr<64>, i8>
// CHECK-LABEL: sea.program @kept
// CHECK: %[[M0:.*]] = sea.mem.nondet
// CHECK: sea.memmove %[[M0]]{{.*}}, %{{.*}}
// CHECK: %[[MV:.*]] = sea.memmove {{.*}}, %{{.*}}
// CHECK-NOT: sea.memmove
// CHECK: sea.load %[[MV]][
// CHECK: sea.halt
sea.program @kept {
  %m0 = sea.mem.nondet "m" [1] : !m
  %c16 = sea.const 16 : i64
  %p = sea.alloca %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %eight = sea.const 8 : i64
  %d = sea.ptr.add %p, %eight : !sea.ptr<64>, i64
  %five = sea.const 5 : i64
  %m1 = sea.memmove %m0[%d], %m0[%p], %five : !m
  %n = sea.nondet "n" [3] : i64
  %m2 = sea.memmove %m1[%d], %m1[%p], %n : !m
  %zero = sea.const 0 : i64
  %m3 = sea.memmove %m2[%d], %m2[%p], %zero : !m
  %x = sea.load %m3[%d] : !m -> i8
  %z = sea.const 0 : i8
  %ok = sea.icmp ne, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// -----

// Keyed memory (fat-pointer companions): one whole cell per byte address,
// copied cell by cell, but only when its mirror (the first non-keyed
// memmove after it in the block with the same addr, srcAddr and len) is
// expanded. %k1 mirrors %b1 (expanded); %k2 mirrors %w1, a word layout
// (kept, so %k2 too); %k3 has no mirror (kept).
!b = !sea.mem<!sea.addr<64>, i8>
!k = !sea.mem<!sea.addr<64>, i64, keyed>
!w = !sea.mem<!sea.addr<64>, i32, aligned>
// CHECK-LABEL: sea.program @layouts
// CHECK-SAME: expanded = 2
// CHECK-SAME: "kept-companion-mirror" = 2
// CHECK-SAME: "kept-word-layout" = 1
// CHECK: sea.load %{{.*}} -> i64
// CHECK: sea.load %{{.*}} -> i64
// CHECK: sea.store {{.*}} : !sea.mem<!sea.addr<64>, i64, keyed>, i64
// CHECK: sea.store {{.*}} : !sea.mem<!sea.addr<64>, i64, keyed>, i64
// CHECK: sea.store {{.*}} : !sea.mem<!sea.addr<64>, i8>, i8
// CHECK: sea.store {{.*}} : !sea.mem<!sea.addr<64>, i8>, i8
// CHECK: %[[K2:.*]] = sea.memmove {{.*}} : !sea.mem<!sea.addr<64>, i64, keyed>
// CHECK: sea.memmove {{.*}} : !sea.mem<!sea.addr<64>, i32, aligned>
// CHECK: sea.memmove %[[K2]]{{.*}} : !sea.mem<!sea.addr<64>, i64, keyed>
sea.program @layouts {
  %a = sea.segment "o" base 4096 : i64 size 32 : i64 align 8 kind "global" : !sea.addr<64>
  %k0 = sea.mem.nondet "k" [1] : !k
  %w0 = sea.mem.nondet "w" [2] : !w
  %b0 = sea.mem.nondet "b" [3] : !b
  %two = sea.const 2 : i64
  %four = sea.const 4 : i64
  %eight = sea.const 8 : i64
  %c16 = sea.const 16 : i64
  %d = sea.addr.add %a, %eight : !sea.addr<64>, i64
  %e = sea.addr.add %a, %c16 : !sea.addr<64>, i64
  %k1 = sea.memmove %k0[%d], %k0[%a], %two inplace : !k
  %b1 = sea.memmove %b0[%d], %b0[%a], %two inplace : !b
  %k2 = sea.memmove %k1[%e], %k1[%a], %four inplace : !k
  %w1 = sea.memmove %w0[%e], %w0[%a], %four inplace : !w
  %k3 = sea.memmove %k2[%a], %k2[%e], %four inplace : !k
  %x = sea.load %k3[%d] : !k -> i64
  %y = sea.load %w1[%e] : !w -> i32
  %z = sea.load %b1[%d] : !b -> i8
  %y64 = sea.zext %y : i32 to i64
  %z64 = sea.zext %z : i8 to i64
  %s = sea.add %y64, %z64 : i64
  %ok = sea.icmp eq, %x, %s : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
