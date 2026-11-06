// forward-operand: a value op's result -> its first operand of the same
// type (add -> lhs, ptr.add -> ptr, select -> true value);
// zero-const: an integer op with no operand of its type (icmp, zext) ->
// sea.const 0. Nondets and constants stay.
// RUN: sea-opt %s --sea-reduce-apply="pattern=forward-operand" | sea-opt | FileCheck %s --check-prefix=FWD
// RUN: sea-opt %s --sea-reduce-apply="pattern=zero-const" | sea-opt | FileCheck %s --check-prefix=ZERO

// FWD-LABEL: sea.program @main
// FWD:       %[[A:.*]] = sea.nondet "nd" [0] : i8
// FWD-NOT:   sea.add
// FWD-NOT:   sea.ptr.add
// FWD-NOT:   sea.select
// FWD:       sea.zext %[[A]]
// FWD:       sea.icmp

// ZERO-LABEL: sea.program @main
// ZERO-NOT:   sea.icmp
// ZERO-NOT:   sea.zext
// ZERO:       sea.add
// ZERO:       sea.assert

sea.program @main {
  %a = sea.nondet "nd" [0] : i8
  %c1 = sea.const 1 : i8
  %b = sea.add %a, %c1 : i8
  %g = sea.global @g size 16 align 8 : !sea.ptr<64>
  %w = sea.zext %b : i8 to i64
  %p = sea.ptr.add %g, %w : !sea.ptr<64>, i64
  %d = sea.icmp ult, %b, %c1 : i8
  %s = sea.select %d, %p, %g : !sea.ptr<64>
  %n = sea.ptr.null : !sea.ptr<64>
  %e = sea.ptr.cmp ne, %s, %n : !sea.ptr<64>
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
