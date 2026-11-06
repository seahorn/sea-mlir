// RUN: sea-opt %s -split-input-file -verify-diagnostics

sea.program @wide_addr {
  // expected-error@+2 {{address width must be in 1..64, got 65}}
  // expected-error@+1 {{failed to parse Sea_MemType parameter 'index'}}
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<65>, i8>
  sea.halt
}

// -----

sea.program @wrong_index {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
  %z = sea.const 0 : i32
  %a = sea.addr.from_int %z : i32 to !sea.addr<32>
  // expected-error@+1 {{use of value '%a' expects different type than prior uses}}
  %x = sea.load %m[%a] : !sea.mem<!sea.addr<64>, i8> -> i8
  // expected-note@-3 {{prior use here}}
  sea.halt
}

// -----

sea.program @wrong_cell {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<8>, i8>
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  // expected-error@+1 {{result type 'i12' does not fit cells 'i8'}}
  %x = sea.load %m[%a] : !sea.mem<!sea.addr<8>, i8> -> i12
  sea.halt
}

// -----

sea.program @offset_width {
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %o = sea.const 1 : i16
  // expected-error@+1 {{offset must have the address width}}
  %b = sea.addr.add %a, %o : !sea.addr<8>, i16
  sea.halt
}

// -----

sea.program @no_mul {
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  // expected-error@+1 {{expected builtin.integer, but found '!sea.addr<8>'}}
  %b = sea.mul %a, %a : !sea.addr<8>
  sea.halt
}

// -----

sea.program @dup_site {
  // expected-note@+1 {{previous use here}}
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<8>, i8>
  // expected-error@+1 {{duplicate nondet site 0}}
  %x = sea.nondet "nondet_u8" [0] : i8
  sea.halt
}

// -----

sea.program @keyed_unit {
  // expected-error@+1 {{the keyed layout needs byte addresses (unit = 8)}}
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<32, unit = 32>, i64, keyed>
  sea.halt
}

// -----

sea.program @keyed_bytes {
  // expected-error@+1 {{a keyed cell is whole bytes, got 'i12'}}
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i12, keyed>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
