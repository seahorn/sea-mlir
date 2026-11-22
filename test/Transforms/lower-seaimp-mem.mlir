// RUN: sea-opt %s --sea-lower-seaimp | FileCheck %s
// RUN: sea-opt %s --sea-lower-seaimp | FileCheck %s --check-prefix=NOIMP
// RUN: sea-opt %s --sea-vc-lower | FileCheck %s --check-prefix=NOIMP

// sea-lower-seaimp on memory variables (regions): each region access is
// get / the sea op inplace / set, then Mem2Reg builds the Mem-SSA chain
// (docs/design/SeaImp.md).
// NOIMP-NOT: seaimp.

!m = !sea.mem<!sea.ptr<64>, i8>

// Section 10, example 2: one store on each path, a load at the join. The
// region starts as sea.mem.nondet with the variable's name and site; the
// join gets a memory block argument.
// CHECK-LABEL: sea.program @ptr
// CHECK-NEXT: %[[M0:.*]] = sea.mem.nondet "mem" [0] : !sea.mem<!sea.ptr<64>, i8>
// CHECK: %[[P:.*]] = sea.alloca %{{.*}} align 4 [1] : i64 -> !sea.ptr<64>
// CHECK: %[[M1:.*]] = sea.store %[[M0]][%[[P]]], %{{.*}} inplace align 4 : !sea.mem<!sea.ptr<64>, i8>, i32
// CHECK: sea.cond_br %{{.*}}, ^[[T:.*]], ^[[J:.*]](%[[M1]] : !sea.mem<!sea.ptr<64>, i8>)
// CHECK: ^[[T]]:
// CHECK: %[[M2:.*]] = sea.store %[[M1]][%[[P]]], %{{.*}} inplace align 4
// CHECK-NEXT: sea.br ^[[J]](%[[M2]] : !sea.mem<!sea.ptr<64>, i8>)
// CHECK: ^[[J]](%[[M3:.*]]: !sea.mem<!sea.ptr<64>, i8>):
// CHECK-NEXT: sea.load %[[M3]][%[[P]]] align 4 : !sea.mem<!sea.ptr<64>, i8> -> i32
sea.program @ptr {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %c4  = sea.const 4 : i64
  %p   = seaimp.alloca %mem, %c4 align 4 [1] : !seaimp.var<!m>, i64 -> !sea.ptr<64>
  %v5  = sea.const 5 : i32
  seaimp.store %mem[%p], %v5 align 4 : !seaimp.var<!m>, i32
  %c   = sea.nondet "nd_bool" [2] : i1
  sea.cond_br %c, ^bb1, ^bb2
^bb1:
  %v7 = sea.const 7 : i32
  seaimp.store %mem[%p], %v7 align 4 : !seaimp.var<!m>, i32
  sea.br ^bb2
^bb2:
  %x  = seaimp.load %mem[%p] align 4 : !seaimp.var<!m> -> i32
  %ok = sea.icmp sge, %x, %v5 : i32
  sea.assert %ok
  sea.halt
}

// Section 10, example 3: bulk ops form a linear chain; memmove reads and
// writes the same version; memhavoc keeps its site.
// CHECK-LABEL: sea.program @bulk
// CHECK-NEXT: %[[M0:.*]] = sea.mem.nondet "mem" [0]
// CHECK: %[[M1:.*]] = sea.memset %[[M0]][%[[BUF:.*]]], %{{.*}}, %{{.*}} inplace : !sea.mem<!sea.ptr<64>, i8>
// CHECK-NEXT: %[[M2:.*]] = sea.memmove %[[M1]][%[[DST:.*]]], %[[M1]][%[[BUF]]], %{{.*}} inplace align 8 src_align 8 : !sea.mem<!sea.ptr<64>, i8>
// CHECK-NEXT: %[[M3:.*]] = sea.memhavoc %[[M2]][%[[BUF]]], %{{.*}} [5] inplace : !sea.mem<!sea.ptr<64>, i8>
// CHECK-NEXT: sea.load %[[M3]][%[[DST]]] : !sea.mem<!sea.ptr<64>, i8> -> i64
sea.program @bulk {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %z8 = sea.const 0 : i8
  %buf = seaimp.alloca %mem, %c16 align 8 [1] : !seaimp.var<!m>, i64 -> !sea.ptr<64>
  %dst = seaimp.alloca %mem, %c8 align 8 [2] : !seaimp.var<!m>, i64 -> !sea.ptr<64>
  seaimp.memset %mem[%buf], %z8, %c16 : !seaimp.var<!m>
  seaimp.memmove %mem[%dst], %mem[%buf], %c8 align 8 src_align 8 : !seaimp.var<!m>
  seaimp.memhavoc %mem[%buf], %c16 [5] : !seaimp.var<!m>
  %x = seaimp.load %mem[%dst] : !seaimp.var<!m> -> i64
  %z64 = sea.const 0 : i64
  %ok = sea.icmp eq, %x, %z64 : i64
  sea.assert %ok
  sea.halt
}

// Section 10, example 4: a memmove across two regions reads the source's
// version and updates the destination's chain only.
// CHECK-LABEL: sea.program @two_regions
// CHECK-DAG: %[[H0:.*]] = sea.mem.nondet "heap" [0]
// CHECK-DAG: %[[S0:.*]] = sea.mem.nondet "stack" [1]
// CHECK: %[[H1:.*]] = sea.memmove %[[H0]][%{{.*}}], %[[S0]][%{{.*}}], %{{.*}} inplace
// CHECK-NEXT: sea.load %[[H1]]
// CHECK-NEXT: sea.load %[[S0]]
sea.program @two_regions {
  %heap  = seaimp.var "heap"  [0] : !seaimp.var<!m>
  %stack = seaimp.var "stack" [1] : !seaimp.var<!m>
  %c8 = sea.const 8 : i64
  %buf = seaimp.alloca %stack, %c8 align 8 [2] : !seaimp.var<!m>, i64 -> !sea.ptr<64>
  %dst = sea.malloc %c8 align 8 [3] : i64 -> !sea.ptr<64>
  seaimp.memmove %heap[%dst], %stack[%buf], %c8 : !seaimp.var<!m>
  %x = seaimp.load %heap[%dst] : !seaimp.var<!m> -> i64
  %y = seaimp.load %stack[%buf] : !seaimp.var<!m> -> i64
  %ok = sea.icmp eq, %x, %y : i64
  sea.assert %ok
  sea.halt
}

// A store in a loop: the header gets a memory block argument fed by the
// entry and the back edge; the exit reads it.
// CHECK-LABEL: sea.program @loop
// CHECK: %[[M0:.*]] = sea.mem.nondet "mem" [0]
// CHECK: sea.br ^[[H:bb[0-9]+]]({{.*}}%[[M0]]{{.*}})
// CHECK: ^[[H]]({{.*}}%[[MH:[0-9]+]]: !sea.mem<!sea.ptr<64>, i8>{{.*}}):
// CHECK: %[[MB:.*]] = sea.store %[[MH]][%{{.*}}], %{{.*}} inplace
// CHECK: sea.br ^[[H]]({{.*}}%[[MB]]{{.*}})
// CHECK: sea.load %[[MH]]
sea.program @loop {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %i = seaimp.var "i" [1] : !seaimp.var<i64>
  %c4 = sea.const 4 : i64
  %a = seaimp.alloca %mem, %c4 align 1 [2] : !seaimp.var<!m>, i64 -> !sea.ptr<64>
  %z = sea.const 0 : i64
  seaimp.set %i, %z : !seaimp.var<i64>
  sea.br ^head
^head:
  %iv = seaimp.get %i : !seaimp.var<i64>
  %lt = sea.icmp ult, %iv, %c4 : i64
  sea.cond_br %lt, ^body, ^exit
^body:
  %q = sea.ptr.add %a, %iv : !sea.ptr<64>, i64
  %b = sea.const 1 : i8
  seaimp.store %mem[%q], %b : !seaimp.var<!m>, i8
  %one = sea.const 1 : i64
  %n = sea.add %iv, %one : i64
  seaimp.set %i, %n : !seaimp.var<i64>
  sea.br ^head
^exit:
  %x = seaimp.load %mem[%a] : !seaimp.var<!m> -> i8
  %one8 = sea.const 1 : i8
  %ok = sea.icmp eq, %x, %one8 : i8
  sea.assert %ok
  sea.halt
}

// Tracking and printing: tracked updates keep `tracked`; reset_modified is
// an update, is_modified and the strong print.mem read the current version;
// a read in a sea.box.all body reads the version before the box (its get is
// put before the box op).
// CHECK-LABEL: sea.program @spec
// CHECK: %[[M0:.*]] = sea.mem.nondet "mem" [0]
// CHECK: %[[M1:.*]] = sea.store %[[M0]][%[[P:.*]]], %{{.*}} inplace tracked
// CHECK-NEXT: %[[M2:.*]] = sea.memset %[[M1]][%[[P]]], %{{.*}}, %{{.*}} inplace tracked
// CHECK-NEXT: %[[M3:.*]] = sea.memhavoc %[[M2]][%[[P]]], %{{.*}} [3] inplace tracked
// CHECK-NEXT: %[[M4:.*]] = sea.memmove %[[M3]][%[[Q:.*]]], %[[M3]][%[[P]]], %{{.*}} inplace tracked
// CHECK-NEXT: %[[F:.*]] = sea.mem.is_modified %[[M4]][%[[Q]]]
// CHECK-NEXT: %[[M5:.*]] = sea.mem.reset_modified %[[M4]][%[[Q]]] inplace
// CHECK-NEXT: sea.print.mem "v" %[[M5]][%[[Q]]], %{{.*}} : !sea.mem<!sea.ptr<64>, i8>, i64 -> i8 {fmt = "x", offset = 1 : i64}
// CHECK-NEXT: sea.print.mem "w" %[[M5]][%[[Q]]] : !sea.mem<!sea.ptr<64>, i8> -> i16 {count = 2 : i64}
// CHECK-NEXT: sea.box.all
// CHECK: sea.load %[[M5]]
// CHECK: sea.mem.is_modified %[[M5]]
sea.program @spec {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %c4 = sea.const 4 : i64
  %p = seaimp.malloc %mem, %c4 align 4 [1] : !seaimp.var<!m>, i64 -> !sea.ptr<64>
  %q = seaimp.global %mem @g size 4 align 4 : !seaimp.var<!m> -> !sea.ptr<64>
  %z = sea.const 0 : i8
  seaimp.store %mem[%p], %z tracked : !seaimp.var<!m>, i8
  seaimp.memset %mem[%p], %z, %c4 tracked : !seaimp.var<!m>
  seaimp.memhavoc %mem[%p], %c4 [3] tracked : !seaimp.var<!m>
  seaimp.memmove %mem[%q], %mem[%p], %c4 tracked : !seaimp.var<!m>
  %f = seaimp.is_modified %mem[%q] : !seaimp.var<!m>
  seaimp.reset_modified %mem[%q] : !seaimp.var<!m>
  seaimp.print.mem "v" %mem[%q], %c4 fmt "x" offset 1 : !seaimp.var<!m>, i64 -> i8
  seaimp.print.mem "w" %mem[%q] count 2 : !seaimp.var<!m> -> i16
  %all = sea.box.all (%i) = (0) to (%c4) {
    %r = sea.ptr.add %q, %i : !sea.ptr<64>, i64
    %x = seaimp.load %mem[%r] : !seaimp.var<!m> -> i8
    %g = seaimp.is_modified %mem[%r] : !seaimp.var<!m>
    %e = sea.icmp eq, %x, %z : i8
    %o = sea.or %e, %g : i1
    sea.yield %o
  }
  %ok = sea.and %all, %f : i1
  sea.assert %ok
  sea.halt
}

// The allocation forms: seaimp.malloc and seaimp.global become sea.malloc
// and sea.global with their attributes; the region is dropped.
// CHECK-LABEL: sea.program @objects
// CHECK-NOT: sea.mem.nondet
// CHECK: sea.malloc %{{.*}} align 8 [1] label "heap" : i64 -> !sea.ptr<64>
// CHECK: sea.global @g size 4 align 4 label "g" : !sea.ptr<64>
sea.program @objects {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %c4 = sea.const 4 : i64
  %p = seaimp.malloc %mem, %c4 align 8 [1] label "heap" : !seaimp.var<!m>, i64 -> !sea.ptr<64>
  %g = seaimp.global %mem @g size 4 align 4 label "g" : !seaimp.var<!m> -> !sea.ptr<64>
  %ok = sea.ptr.cmp ne, %p, %g : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}

// A region used only by allocations: the variable goes away, each
// seaimp.alloca becomes sea.alloca with its size, align, site and label.
// CHECK-LABEL: sea.program @alloca_only
// CHECK-NOT: sea.mem.nondet
// CHECK: sea.alloca %{{.*}} align 8 [1] label "buf" : i64 -> !sea.ptr<64>
// CHECK: sea.ptr.cmp
sea.program @alloca_only {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %c4 = sea.const 4 : i64
  %p = seaimp.alloca %mem, %c4 align 8 [1] label "buf" : !seaimp.var<!m>, i64 -> !sea.ptr<64>
  %z = sea.ptr.null : !sea.ptr<64>
  %ok = sea.ptr.cmp ne, %p, %z : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
