// RUN: sea-opt %s -split-input-file --pass-pipeline='builtin.module(sea.program(sea-scalarize-structs))' | FileCheck %s --check-prefix=SCALAR
// RUN: sea-opt %s -split-input-file --pass-pipeline='builtin.module(sea.program(sea-widen-memory{k=8 report=true}))' 2>/dev/null | FileCheck %s --check-prefix=WIDEN
// RUN: sea-opt %s -split-input-file --pass-pipeline='builtin.module(sea.program(sea-merge-asserts))' | FileCheck %s --check-prefix=MERGE

// Strong prints are users like any other: sea-scalarize-structs makes a
// struct print show the leaves; sea-widen-memory widens a memory a print
// reads; a print between the assert and the halt keeps BMC form.

// SCALAR-LABEL: sea.program @structs
// SCALAR: %[[X:.*]] = sea.nondet
// SCALAR: %[[Y:.*]] = sea.const 7
// SCALAR: sea.print "pt" %[[X]], %[[Y]] : i32, i8 {names = ["x", "y"], type = !sea.struct<i32, i8>}
// WIDEN-LABEL: sea.program @structs
// MERGE-LABEL: sea.program @structs
sea.program @structs {
  %x = sea.nondet "nondet_u32" [0] : i32
  %y = sea.const 7 : i8
  %s = sea.struct.create %x, %y : i32, i8 -> !sea.struct<i32, i8>
  sea.print "pt" %s : !sea.struct<i32, i8> {names = ["x", "y"]}
  sea.halt
}

// -----

// WIDEN-LABEL: sea.program @widen
// WIDEN: sea.print.mem "bytes" %{{.*}}[%{{.*}}] : !sea.mem<!sea.addr<64>, i64, aligned> -> i8
!m = !sea.mem<!sea.addr<64>, i8>
sea.program @widen {
  %m0 = sea.mem.nondet "m" [1] : !m
  %a = sea.segment "heap.2" base 64 : i64 size 16 : i64 align 8 kind "heap" : !sea.addr<64>
  %v = sea.const 5 : i64
  %m1 = sea.store %m0[%a], %v inplace : !m, i64
  sea.print.mem "bytes" %m1[%a] : !m -> i8 {fmt = "hexdump", count = 3}
  sea.halt
}

// -----

// MERGE-LABEL: sea.program @bmc
// MERGE-NOT: sea.cond_br
// MERGE: sea.assert
// MERGE-NEXT: sea.print "last"
// MERGE-NEXT: sea.halt
sea.program @bmc {
  %t = sea.const 1 : i1
  sea.assert %t
  sea.print "last"
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
