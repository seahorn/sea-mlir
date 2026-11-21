// sea-check-inb drops the program's own asserts but keeps premise asserts
// (sea.premise): they are not the property, and nothing assumes them.
// RUN: sea-opt %s --sea-check-inb 2>/dev/null | FileCheck %s
// CHECK-LABEL: sea.program @p
// CHECK: sea.assert {{.*}}{sea.premise = "symbolic-size-bound"}
// CHECK: sea.assert {{.*}}{sea.inb_check = {{[0-9]+}} : i64}
// CHECK-NOT: sea.assert
// CHECK: sea.halt
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @p {
  %m0 = sea.mem.nondet "m" [0] : !m
  %k = sea.nondet "k" [1] : i64
  %c16 = sea.const 16 : i64
  %fits = sea.icmp ule, %k, %c16 : i64
  sea.assert %fits {sea.premise = "symbolic-size-bound"}
  %a = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [3] : i64
  %q = sea.ptr.add %a, %i : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] : !m -> i8
  %c5 = sea.const 5 : i8
  %ok = sea.icmp eq, %x, %c5 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
