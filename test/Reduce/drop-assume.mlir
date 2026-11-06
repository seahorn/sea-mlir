// drop-assume: the first sea.assume is erased, the second stays.
// RUN: sea-opt %s --sea-reduce-apply="pattern=drop-assume limit=1" | sea-opt | FileCheck %s

// CHECK-LABEL: sea.program @main
// CHECK:     %[[B:.*]] = sea.icmp ult
// CHECK-NOT: sea.assume
// CHECK:     sea.assume %[[B]]
// CHECK-NOT: sea.assume
// CHECK:     sea.assert

sea.program @main {
  %a = sea.nondet "nd" [0] : i8
  %c1 = sea.const 1 : i8
  %c = sea.icmp eq, %a, %c1 : i8
  sea.assume %c
  %b = sea.icmp ult, %a, %c1 : i8
  sea.assume %b
  %z = sea.icmp ne, %a, %c1 : i8
  sea.assert %z
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
