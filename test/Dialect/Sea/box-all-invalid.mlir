// RUN: sea-opt %s -split-input-file -verify-diagnostics
// The sea.box.all verifier (docs/design/BoxAll.md): the box and the body
// it accepts; everything else is refused.

sea.program @dims {
  %n = sea.const 2 : i64
  // expected-error@+1 {{needs 1 to 2 dimensions, got 3}}
  %all = sea.box.all (%i, %j, %k) = (0, 0, 0) to (%n, %n, %n) {
    %t = sea.const 1 : i1
    sea.yield %t
  }
  sea.assert %all
  sea.halt
}

// -----

sea.program @lower {
  %n = sea.const 2 : i64
  // expected-error@+1 {{lower bound of dimension 0 must be in [0, 2^63), got -1}}
  %all = sea.box.all (%i) = (-1) to (%n) {
    %t = sea.const 1 : i1
    sea.yield %t
  }
  sea.assert %all
  sea.halt
}

// -----

sea.program @step {
  %n = sea.const 2 : i64
  // expected-error@+1 {{step of dimension 0 must be in [1, 2^63), got 0}}
  %all = sea.box.all (%i) = (0) to (%n) step (0) {
    %t = sea.const 1 : i1
    sea.yield %t
  }
  sea.assert %all
  sea.halt
}

// -----

sea.program @mismatch {
  %n = sea.const 2 : i64
  // expected-error@+1 {{needs one lower bound, step and upper bound per dimension}}
  %all = sea.box.all (%i) = (0, 0) to (%n) {
    %t = sea.const 1 : i1
    sea.yield %t
  }
  sea.assert %all
  sea.halt
}

// -----

!m = !sea.mem<!sea.addr<64>, i8>
sea.program @store {
  %n = sea.const 2 : i64
  %m = sea.mem.nondet "m" [0] : !m
  %z = sea.const 0 : i8
  %all = sea.box.all (%i) = (0) to (%n) {
    %a = sea.addr.from_int %i : i64 to !sea.addr<64>
    // expected-error@+1 {{not allowed in a sea.box.all body: a memory result}}
    %m1 = sea.store %m[%a], %z : !m, i8
    %t = sea.const 1 : i1
    sea.yield %t
  }
  sea.assert %all
  sea.halt
}

// -----

sea.program @nondet {
  %n = sea.const 2 : i64
  %all = sea.box.all (%i) = (0) to (%n) {
    // expected-error@+1 {{not allowed in a sea.box.all body: an op with effects}}
    %x = sea.nondet "x" [0] : i1
    sea.yield %x
  }
  sea.assert %all
  sea.halt
}

// -----

sea.program @assert {
  %n = sea.const 2 : i64
  %all = sea.box.all (%i) = (0) to (%n) {
    %t = sea.const 1 : i1
    // expected-error@+1 {{not allowed in a sea.box.all body: an op with effects}}
    sea.assert %t
    sea.yield %t
  }
  sea.assert %all
  sea.halt
}

// -----

sea.program @nested {
  %n = sea.const 2 : i64
  %all = sea.box.all (%i) = (0) to (%n) {
    // expected-error@+1 {{not allowed in a sea.box.all body: a nested region}}
    %in = sea.box.all (%j) = (0) to (%n) {
      %t = sea.const 1 : i1
      sea.yield %t
    }
    sea.yield %in
  }
  sea.assert %all
  sea.halt
}

// -----

sea.program @object {
  %n = sea.const 2 : i64
  %all = sea.box.all (%i) = (0) to (%n) {
    // expected-error@+1 {{not allowed in a sea.box.all body: an op that names an object}}
    %g = sea.global @g size 4 align 4 : !sea.ptr<64>
    %t = sea.const 1 : i1
    sea.yield %t
  }
  sea.assert %all
  sea.halt
}

// -----

sea.program @no_yield {
  %n = sea.const 2 : i64
  %all = sea.box.all (%i) = (0) to (%n) {
    // expected-error@+1 {{block with no terminator}}
    %t = sea.const 1 : i1
  }
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
