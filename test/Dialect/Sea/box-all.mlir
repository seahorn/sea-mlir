// RUN: sea-opt %s | sea-opt | FileCheck %s
// sea.box.all round-trips (docs/design/BoxAll.md): one and two dimensions,
// `step` printed only when a step is not 1, values from outside the box,
// loads of memory defined outside it.

!m = !sea.mem<!sea.ptr<64>, i8>

// CHECK-LABEL: sea.program @one
// CHECK: sea.box.all (%{{.*}}) = (0) to (%{{.*}}) {
// CHECK: sea.load
// CHECK: sea.yield %{{.*}}
// CHECK-NOT: step
sea.program @one {
  %n = sea.const 4 : i64
  %c16 = sea.const 16 : i64
  %m = sea.mem.nondet "m" [0] : !m
  %a = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %z = sea.const 0 : i8
  %all = sea.box.all (%i) = (0) to (%n) {
    %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
    %x = sea.load %m[%p] : !m -> i8
    %e = sea.icmp eq, %x, %z : i8
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// CHECK-LABEL: sea.program @two
// CHECK: sea.box.all (%{{.*}}, %{{.*}}) = (1, 0) to (%{{.*}}, %{{.*}}) step (2, 1) attributes {note = "x"} {
sea.program @two {
  %n = sea.nondet "n" [0] : i64
  %c3 = sea.const 3 : i64
  %all = sea.box.all (%i, %j) = (1, 0) to (%n, %c3) step (2, 1) attributes {note = "x"} {
    %e = sea.icmp ne, %i, %j : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
