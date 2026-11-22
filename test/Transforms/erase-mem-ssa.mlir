// RUN: sea-opt %s --sea-erase-mem-ssa | FileCheck %s
// RUN: sea-opt %s --sea-erase-mem-ssa | sea-opt --sea-lower-seaimp | FileCheck %s --check-prefix=LOW
// The VC of each program with an assert is the original's (erase, then
// sea-vc --lower): the region forms of tracking, box reads, unreachable code.
// DEFINE: %{ed} = %python %sea_src_root/utils/encoding-diff.py --sea-vc %sea_vc --z3 %z3
// RUN: sea-opt %s --sea-erase-mem-ssa -o %t.erased.mlir
// RUN: %if z3 %{ %{ed} %s %t.erased.mlir --left-flags=--program=tracking --right-flags=--program=tracking | FileCheck %s --check-prefix=IDENT %}
// RUN: %if z3 %{ %{ed} %s %t.erased.mlir --left-flags=--program=box --right-flags=--program=box | FileCheck %s --check-prefix=IDENT %}
// RUN: %if z3 %{ %{ed} %s %t.erased.mlir --left-flags=--program=unreachable --right-flags=--program=unreachable | FileCheck %s --check-prefix=IDENT %}
// RUN: %if z3 %{ %{ed} %s %t.erased.mlir --left-flags=--program=ptr --right-flags=--program=ptr | FileCheck %s --check-prefix=IDENT %}
// IDENT: result: identical

// sea-erase-mem-ssa: the Mem-SSA chain of the program's memory becomes the
// region %mem; sea-lower-seaimp builds the chain again
// (docs/design/SeaImp.md).

!m = !sea.mem<!sea.ptr<64>, i8>

// int o; int *p = &o; *p = 5; if (c) *p = 7; assert(*p >= 5), written in
// sea. The sea.mem.nondet in the entry block becomes the variable (name and
// site); the join's memory argument and the branch operands go away; the
// allocation names the region.
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

// memset(buf, 0, 16); memcpy(dst, buf, 8); havoc(buf, 16): bulk ops;
// memmove reads and writes one region; memhavoc keeps its site.
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

// Modification tracking: tracked updates keep `tracked`, reset_modified and
// is_modified have region forms; the lowering rebuilds the chain.
// CHECK-LABEL: sea.program @tracking
// CHECK: %[[MEM:.*]] = seaimp.var "mem" [0]
// CHECK: seaimp.store %[[MEM]][%[[P:.*]]], %{{.*}} tracked :
// CHECK-NEXT: seaimp.memmove %[[MEM]][%[[Q:.*]]], %[[MEM]][%[[P]]], %{{.*}} tracked :
// CHECK-NEXT: seaimp.reset_modified %[[MEM]][%[[P]]] :
// CHECK-NEXT: %{{.*}} = seaimp.is_modified %[[MEM]][%[[Q]]] :
// CHECK-NOT: seaimp.get
// LOW-LABEL: sea.program @tracking
// LOW: %[[M0:.*]] = sea.mem.nondet "mem" [0]
// LOW: %[[M1:.*]] = sea.store %[[M0]][%{{.*}}], %{{.*}} inplace tracked
// LOW-NEXT: %[[M2:.*]] = sea.memmove %[[M1]][%{{.*}}], %[[M1]][%{{.*}}], %{{.*}} inplace tracked
// LOW-NEXT: %[[M3:.*]] = sea.mem.reset_modified %[[M2]][%{{.*}}] inplace
// LOW-NEXT: sea.mem.is_modified %[[M3]][%{{.*}}]
sea.program @tracking {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %z = sea.const 0 : i8
  %c4 = sea.const 4 : i64
  %p = sea.malloc %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %q = sea.malloc %c4 align 4 [2] : i64 -> !sea.ptr<64>
  %m1 = sea.store %m0[%p], %z inplace tracked : !m, i8
  %m2 = sea.memmove %m1[%q], %m1[%p], %c4 inplace tracked : !m
  %m3 = sea.mem.reset_modified %m2[%p] inplace : !m
  %mod = sea.mem.is_modified %m3[%q] : !m
  sea.assert %mod
  sea.halt
}

// A strong print.mem reads the region at its point (count operand, format,
// offset kept); a weak one names memories and stays.
// CHECK-LABEL: sea.program @print
// CHECK: seaimp.print.mem "v" %[[MEM:.*]][%[[P:.*]]] offset 4 : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>> -> i32
// CHECK-NEXT: seaimp.print.mem "b" %[[MEM]][%[[P]]], %{{.*}} fmt "hexdump" : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>, i64 -> i8
// CHECK-NEXT: sea.print.mem "w" = "nd.0"["p"] : i8
// LOW-LABEL: sea.program @print
// LOW: %[[M1:.*]] = sea.store
// LOW-NEXT: sea.print.mem "v" %[[M1]][%{{.*}}] : !sea.mem<!sea.ptr<64>, i8> -> i32 {offset = 4 : i64}
// LOW-NEXT: sea.print.mem "b" %[[M1]][%{{.*}}], %{{.*}} : !sea.mem<!sea.ptr<64>, i8>, i64 -> i8 {fmt = "hexdump"}
sea.program @print {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c8 = sea.const 8 : i64
  %p = sea.malloc %c8 align 4 [1] : i64 -> !sea.ptr<64>
  %z = sea.const 0 : i8
  %m1 = sea.store %m0[%p], %z inplace : !m, i8
  sea.print.mem "v" %m1[%p] : !m -> i32 {offset = 4}
  sea.print.mem "b" %m1[%p], %c8 : !m, i64 -> i8 {fmt = "hexdump"}
  sea.print.mem "w" = "nd.0"["p"] : i8
  sea.halt
}

// Reads in a sea.box.all body become region reads in the body; the
// lowering reads the version before the box.
// CHECK-LABEL: sea.program @box
// CHECK: %[[MEM:.*]] = seaimp.var "m" [0]
// CHECK: seaimp.store %[[MEM]]
// CHECK-NEXT: sea.box.all
// CHECK: seaimp.load %[[MEM]][%{{.*}}] : !seaimp.var<!sea.mem<!sea.ptr<64>, i8>> -> i8
// CHECK: seaimp.is_modified %[[MEM]][%{{.*}}] :
// CHECK-NOT: seaimp.get
// LOW-LABEL: sea.program @box
// LOW: %[[M1:.*]] = sea.store
// LOW-NEXT: sea.box.all
// LOW: sea.load %[[M1]]
// LOW: sea.mem.is_modified %[[M1]]
sea.program @box {
  %m = sea.mem.nondet "m" [0] : !m
  %n = sea.const 16 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %z = sea.const 0 : i8
  %m1 = sea.store %m[%p], %z inplace : !m, i8
  %all = sea.box.all (%i) = (0) to (%n) {
    %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
    %x = sea.load %m1[%q] : !m -> i8
    %f = sea.mem.is_modified %m1[%q] : !m
    %e = sea.icmp eq, %x, %z : i8
    %r = sea.or %e, %f : i1
    sea.yield %r
  }
  sea.assert %all
  sea.halt
}

// An unreachable block is rewritten but not checked: its load of an
// earlier version reads the region (nothing executes it).
// CHECK-LABEL: sea.program @unreachable
// CHECK: seaimp.store
// CHECK: ^bb1:  // no predecessors
// CHECK-NEXT: seaimp.load
// LOW-LABEL: sea.program @unreachable
// LOW: ^bb1:  // no predecessors
// LOW-NEXT: sea.load
sea.program @unreachable {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %p = sea.ptr.nondet "p" [1] : !sea.ptr<64>
  %z = sea.const 0 : i8
  %m1 = sea.store %m0[%p], %z inplace : !m, i8
  %x = sea.load %m1[%p] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
^dead:
  %y = sea.load %m0[%p] : !m -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
