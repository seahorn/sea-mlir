// RUN: sea-opt %s --sea-erase-mem-ssa | FileCheck %s
// RUN: sea-opt %s --sea-erase-mem-ssa | sea-opt --sea-lower-seaimp | FileCheck %s --check-prefix=LOW

// sea-erase-mem-ssa: the Mem-SSA chain of the program's memory becomes the
// region %mem; sea-lower-seaimp builds the chain again
// (docs/design/SeaImp.md).

!m = !sea.mem<!sea.ptr<64>, i8>

// Section 10, example 2, written in sea. The sea.mem.nondet in the entry
// block becomes the variable (name and site); the join's memory argument
// and the branch operands go away; the allocation names the region.
// CHECK-LABEL: sea.program @ptr
// CHECK-NEXT: %[[MEM:.*]] = seaimp.var "mem" [0] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>
// CHECK-NOT: sea.mem.nondet
// CHECK: %[[P:.*]] = seaimp.alloca %[[MEM]], %{{.*}} align 4 [1] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>, i64 -> !sea.ptr<64>
// CHECK: seaimp.store %[[MEM]][%[[P]]], %{{.*}} align 4 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>, i32
// CHECK: sea.cond_br %{{.*}}, ^[[T:bb[0-9]+]], ^[[J:bb[0-9]+]]{{$}}
// CHECK: seaimp.store %[[MEM]][%[[P]]]
// CHECK-NEXT: sea.br ^[[J]]{{$}}
// CHECK: ^[[J]]:
// CHECK-NEXT: seaimp.load %[[MEM]][%[[P]]] align 4 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>> -> i32
// Lowered again: the chain and the join argument of the original.
// LOW-LABEL: sea.program @ptr
// LOW-NEXT: %[[M0:.*]] = sea.mem.nondet "mem" [0]
// LOW: %[[M1:.*]] = sea.store %[[M0]][%{{.*}}], %{{.*}} inplace align 4
// LOW: sea.cond_br %{{.*}}, ^{{.*}}, ^[[J:bb[0-9]+]](%[[M1]] : !sea.mem<!sea.ptr<64>, i8>)
// LOW: %[[M2:.*]] = sea.store %[[M1]]
// LOW-NEXT: sea.br ^[[J]](%[[M2]] : !sea.mem<!sea.ptr<64>, i8>)
// LOW: ^[[J]](%[[M3:.*]]: !sea.mem<!sea.ptr<64>, i8>):
// LOW-NEXT: sea.load %[[M3]]
sea.program @ptr {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c4 = sea.const 4 : i64
  %p  = sea.alloca %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %v5 = sea.const 5 : i32
  %m1 = sea.store %m0[%p], %v5 inplace align 4 : !m, i32
  %c  = sea.nondet "nd_bool" [2] : i1
  sea.cond_br %c, ^bb1, ^bb2(%m1 : !m)
^bb1:
  %v7 = sea.const 7 : i32
  %m2 = sea.store %m1[%p], %v7 inplace align 4 : !m, i32
  sea.br ^bb2(%m2 : !m)
^bb2(%m3 : !m):
  %x  = sea.load %m3[%p] align 4 : !m -> i32
  %ok = sea.icmp sge, %x, %v5 : i32
  sea.assert %ok
  sea.halt
}

// Section 10, example 3: bulk ops; memmove reads and writes one region;
// memhavoc keeps its site.
// CHECK-LABEL: sea.program @bulk
// CHECK: seaimp.memset %[[MEM:.*]][%{{.*}}], %{{.*}}, %{{.*}} : !seaimp.var
// CHECK-NEXT: seaimp.memmove %[[MEM]][%{{.*}}], %[[MEM]][%{{.*}}], %{{.*}} src_align 8 : !seaimp.var
// CHECK-NEXT: seaimp.memhavoc %[[MEM]][%{{.*}}], %{{.*}} [5] : !seaimp.var
// LOW-LABEL: sea.program @bulk
// LOW: %[[M1:.*]] = sea.memset %{{.*}} inplace
// LOW-NEXT: %[[M2:.*]] = sea.memmove %[[M1]][%{{.*}}], %[[M1]][%{{.*}}], %{{.*}} inplace src_align 8
// LOW-NEXT: %[[M3:.*]] = sea.memhavoc %[[M2]][%{{.*}}], %{{.*}} [5] inplace
// LOW-NEXT: sea.load %[[M3]]
sea.program @bulk {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %z8 = sea.const 0 : i8
  %buf = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %dst = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %m1 = sea.memset %m0[%buf], %z8, %c16 inplace : !m
  %m2 = sea.memmove %m1[%dst], %m1[%buf], %c8 inplace src_align 8 : !m
  %m3 = sea.memhavoc %m2[%buf], %c16 [5] inplace : !m
  %x = sea.load %m3[%dst] : !m -> i64
  sea.halt
}

// A loop: the header's memory argument goes away; the version the exit
// reads is the header's, which is current there.
// CHECK-LABEL: sea.program @loop
// CHECK-NOT: !sea.mem<!sea.ptr<64>, i8>)
// CHECK: seaimp.store
// CHECK: seaimp.load
// LOW-LABEL: sea.program @loop
// LOW: ^[[H:bb[0-9]+]]({{.*}}%[[MH:[0-9]+]]: !sea.mem<!sea.ptr<64>, i8>{{.*}}):
// LOW: sea.store %[[MH]]
// LOW: sea.load %[[MH]]
sea.program @loop {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c4 = sea.const 4 : i64
  %a = sea.alloca %c4 align 1 [1] : i64 -> !sea.ptr<64>
  %z = sea.const 0 : i64
  sea.br ^head(%z, %m0 : i64, !m)
^head(%i : i64, %mh : !m):
  %lt = sea.icmp ult, %i, %c4 : i64
  sea.cond_br %lt, ^body, ^exit
^body:
  %q = sea.ptr.add %a, %i : !sea.ptr<64>, i64
  %b = sea.const 1 : i8
  %mb = sea.store %mh[%q], %b inplace : !m, i8
  %one = sea.const 1 : i64
  %n = sea.add %i, %one : i64
  sea.br ^head(%n, %mb : i64, !m)
^exit:
  %x = sea.load %mh[%a] : !m -> i8
  sea.halt
}

// A root that is not the variable's (sea.mem.const): it stays and is set;
// the variable is "mem" with a fresh site. Readers without a region op
// (is_modified) and tracked updates go through get / set.
// CHECK-LABEL: sea.program @const_root
// CHECK-NEXT: %[[MEM:.*]] = seaimp.var "mem" [3] :
// CHECK: %[[C:.*]] = sea.mem.const
// CHECK-NEXT: seaimp.set %[[MEM]], %[[C]]
// CHECK: %[[G:.*]] = seaimp.get %[[MEM]]
// CHECK-NEXT: %[[S:.*]] = sea.store %[[G]][%{{.*}}], %{{.*}} inplace tracked
// CHECK-NEXT: seaimp.set %[[MEM]], %[[S]]
// CHECK: %[[G2:.*]] = seaimp.get %[[MEM]]
// CHECK-NEXT: sea.mem.is_modified %[[G2]]
// LOW-LABEL: sea.program @const_root
// LOW-NOT: sea.mem.nondet
// LOW: %[[C:.*]] = sea.mem.const
// LOW: %[[S:.*]] = sea.store %[[C]]{{.*}} tracked
// LOW: sea.mem.is_modified %[[S]]
sea.program @const_root {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c4 = sea.const 4 : i64
  %p = sea.malloc %c4 align 4 [2] : i64 -> !sea.ptr<64>
  %m1 = sea.store %m0[%p], %z inplace tracked : !m, i8
  %mod = sea.mem.is_modified %m1[%p] : !m
  sea.assert %mod
  sea.halt
}

// A load in a sea.box.all body reads a get put before the box (seaimp
// accesses stay in the variable's region).
// CHECK-LABEL: sea.program @box
// CHECK: %[[G:.*]] = seaimp.get
// CHECK-NEXT: sea.box.all
// CHECK: sea.load %[[G]]
// LOW-LABEL: sea.program @box
// LOW: %[[M0:.*]] = sea.mem.nondet "m" [0]
// LOW: sea.box.all
// LOW: sea.load %[[M0]]
sea.program @box {
  %m = sea.mem.nondet "m" [0] : !m
  %n = sea.const 16 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %z = sea.const 0 : i8
  %all = sea.box.all (%i) = (0) to (%n) {
    %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
    %x = sea.load %m[%q] : !m -> i8
    %e = sea.icmp eq, %x, %z : i8
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
