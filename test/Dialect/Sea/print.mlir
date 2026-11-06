// RUN: sea-opt %s | sea-opt | FileCheck %s
// RUN: sea-opt %s --mlir-print-op-generic | sea-opt | FileCheck %s

// Print ops (docs/design/Print.md): weak (names) and strong (operands)
// forms; `label` on allocations, globals and segments.

!m = !sea.mem<!sea.ptr<64>, i8>
// CHECK-LABEL: sea.program @print
sea.program @print {
  %x = sea.nondet "nondet_u64" [0] : i64 loc("x")
  %n = sea.const 4 : i64
  %m = sea.mem.nondet "heap" [1] : !m
  // CHECK: sea.malloc {{.*}} [2] label "node" :
  %p = sea.malloc %n align 8 [2] label "node" : i64 -> !sea.ptr<64>
  // CHECK: sea.global @st size 8 align 8 label "st" :
  %g = sea.global @st size 8 align 8 label "st" : !sea.ptr<64>
  // CHECK: sea.print "reached"
  sea.print "reached"
  // CHECK: sea.print "x" = "x" : i64 {fmt = "s"}
  sea.print "x" = "x" : i64 {fmt = "s"}
  // CHECK: sea.print "x2" %{{.*}} : i64
  sea.print "x2" %x : i64
  // CHECK: sea.print "pt" %{{.*}}, %{{.*}} : i64, !sea.ptr<64> {names = ["a", "b"], type = !sea.struct<i64, !sea.ptr<64>>}
  sea.print "pt" %x, %p : i64, !sea.ptr<64> {type = !sea.struct<i64, !sea.ptr<64>>, names = ["a", "b"]}
  // CHECK: sea.print.scope "pre"
  sea.print.scope "pre"
  // CHECK: sea.print.mem "bal" = "nd.1"["p"] : i64 {fmt = "s", offset = 8 : i64}
  sea.print.mem "bal" = "nd.1"["p"] : i64 {fmt = "s", offset = 8}
  // CHECK: sea.print.mem "buf" = "nd.1"["p"] : i8 {count_ref = "n", fmt = "hexdump"}
  sea.print.mem "buf" = "nd.1"["p"] : i8 {fmt = "hexdump", count_ref = "n"}
  // CHECK: sea.print.mem "buf2" %{{.*}}[%{{.*}}], %{{.*}} : !sea.mem<!sea.ptr<64>, i8>, i64 -> i8 {fmt = "hexdump"}
  sea.print.mem "buf2" %m[%p], %n : !m, i64 -> i8 {fmt = "hexdump"}
  // CHECK: sea.print.mem "next" %{{.*}}[%{{.*}}] : !sea.mem<!sea.ptr<64>, i8> -> !sea.ptr<64>
  sea.print.mem "next" %m[%p] : !m -> !sea.ptr<64>
  // CHECK: sea.print.end "pre"
  sea.print.end "pre"
  sea.halt
}

// CHECK-LABEL: sea.program @segment
sea.program @segment {
  // CHECK: sea.segment "heap.2" base 16 : i32 size 4 : i32 align 4 kind "heap" label "node" : !sea.addr<32>
  %s = sea.segment "heap.2" base 16 : i32 size 4 : i32 align 4 kind "heap" label "node" : !sea.addr<32>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
