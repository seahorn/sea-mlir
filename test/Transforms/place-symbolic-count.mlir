// sea-place-static counts the allocations it bounds by an assume on the
// program (sea.symbolic_size_bound), added to any counts already there.
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static))' -split-input-file 2>/dev/null | FileCheck %s
// CHECK-LABEL: sea.program @fresh
// CHECK-SAME: sea.symbolic_size_bound = {allocs = 2 : i64, room = 4096 : i64}
// CHECK-LABEL: sea.program @added
// CHECK-SAME: sea.symbolic_size_bound = {allocs = 3 : i64, room = 8192 : i64}

sea.program @fresh {
  %k = sea.nondet "k" [0] : i64
  %j = sea.nondet "j" [1] : i64
  %h = sea.malloc %k align 8 [2] : i64 -> !sea.ptr<64>
  %g = sea.malloc %j align 8 [3] : i64 -> !sea.ptr<64>
  sea.halt
}

// -----

sea.program @added attributes {sea.symbolic_size_bound = {allocs = 2 : i64, room = 8192 : i64}} {
  %k = sea.nondet "k" [0] : i64
  %h = sea.malloc %k align 8 [2] : i64 -> !sea.ptr<64>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
