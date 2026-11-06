// RUN: sea-opt %s -split-input-file -verify-diagnostics

sea.program @both {
  %x = sea.const 1 : i8
  // expected-error@+1 {{weak (a name) or strong (operands), not both}}
  "sea.print"(%x) <{tag = "t", ref = "x", type = i8}> : (i8) -> ()
  sea.halt
}

// -----

sea.program @fmt {
  // expected-error@+1 {{unknown format 'q'}}
  sea.print "t" = "x" : i8 {fmt = "q"}
  sea.halt
}

// -----

sea.program @fmt_ptr {
  // expected-error@+1 {{format 's' needs an integer}}
  sea.print "t" = "p" : !sea.ptr<64> {fmt = "s"}
  sea.halt
}

// -----

sea.program @names {
  // expected-error@+1 {{expected 2 names, got 1}}
  sea.print "t" = "s" : !sea.struct<i8, i8> {names = ["a"]}
  sea.halt
}

// -----

sea.program @memtype {
  // expected-error@+1 {{cannot print a value of type}}
  sea.print "t" = "m" : !sea.mem<!sea.ptr<64>, i8>
  sea.halt
}

// -----

sea.program @hexdump {
  // expected-error@+1 {{hexdump reads bytes (type i8)}}
  sea.print.mem "t" = "m"["p"] : i32 {fmt = "hexdump"}
  sea.halt
}

// -----

sea.program @bytes {
  // expected-error@+1 {{the element must be whole bytes}}
  sea.print.mem "t" = "m"["p"] : i12
  sea.halt
}

// -----

sea.program @count0 {
  // expected-error@+1 {{count}}
  sea.print.mem "t" = "m"["p"] : i8 {count = 0}
  sea.halt
}

// -----

sea.program @counts {
  // expected-error@+1 {{count and count_ref exclude each other}}
  sea.print.mem "t" = "m"["p"] : i8 {count = 2, count_ref = "n"}
  sea.halt
}

// -----

sea.program @leaves {
  %x = sea.const 1 : i8
  // expected-error@+1 {{expected 2 leaves, got 1}}
  sea.print "t" %x : i8 {type = !sea.struct<i8, i8>}
  sea.halt
}

// -----

sea.program @tag {
  // expected-error@+1 {{the tag must not be empty}}
  sea.print.scope ""
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
