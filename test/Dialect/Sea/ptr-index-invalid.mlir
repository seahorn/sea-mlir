// RUN: sea-opt %s -split-input-file -verify-diagnostics

sea.program @strides {
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  // expected-error@+1 {{needs one stride per index}}
  %q = sea.ptr.index %a [%b] strides [1, 2] offset 0 : !sea.addr<8>, i8
  sea.halt
}

// -----

sea.program @zero_stride {
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  // expected-error@+1 {{stride 0 is 0}}
  %q = sea.ptr.index %a [%b] strides [0] offset 0 : !sea.addr<8>, i8
  sea.halt
}

// -----

sea.program @stride_bits {
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  // expected-error@+1 {{stride 0 does not fit 8 signed bits}}
  %q = sea.ptr.index %a [%b] strides [200] offset 0 : !sea.addr<8>, i8
  sea.halt
}

// -----

sea.program @offset_bits {
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  // expected-error@+1 {{offset does not fit 8 signed bits}}
  %q = sea.ptr.index %a [%b] strides [1] offset -129 : !sea.addr<8>, i8
  sea.halt
}

// -----

sea.program @wide_index {
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 1 : i16
  // expected-error@+1 {{an index is wider than the pointer}}
  %q = sea.ptr.index %a [%i] strides [1] offset 0 : !sea.addr<8>, i16
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
