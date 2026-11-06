// RUN: sea-opt %s -split-input-file -verify-diagnostics

sea.program @base_width {
  // expected-error@+1 {{base and size must have the address width}}
  %b = sea.segment "stack.0" base 16 : i32 size 16 : i64 align 8 kind "stack" : !sea.addr<64>
  sea.halt
}

// -----

sea.program @size_width {
  // expected-error@+1 {{base and size must have the address width}}
  %b = sea.segment "stack.0" base 16 : i64 size 16 : i32 align 8 kind "stack" : !sea.addr<64>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
