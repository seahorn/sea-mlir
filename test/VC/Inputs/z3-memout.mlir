// Inputs of z3-errors.test: queries whose z3 memory need is set by the
// input, far above z3's baseline (a context and a solver, under 30 MB).
// Run these programs only under a memory limit: without one, z3 can grow
// without bound.
// @check: x * y = 1234567 on 256 bits, then the assert x <u y; z3
// bit-blasts the 256-bit multiplier inside the check (hundreds of MB).
// @sort: two 16777215-bit values (the widest MLIR integer); z3 makes a
// bit-vector sort of width w with an array of w + 1 pointers (128 MB),
// when sea-vc first translates a term of that sort (under --reach:
// evaluating the model while replaying it), outside any check.
sea.program @check {
  %x = sea.nondet "x" [0] : i256
  %y = sea.nondet "y" [1] : i256
  %p = sea.mul %x, %y : i256
  %c = sea.const 1234567 : i256
  %h = sea.icmp eq, %p, %c : i256
  sea.assume %h
  %ok = sea.icmp ult, %x, %y : i256
  sea.assert %ok
  sea.halt
}
sea.program @sort {
  %x = sea.nondet "x" [0] : i16777215
  %y = sea.nondet "y" [1] : i16777215
  %ok = sea.icmp eq, %x, %y : i16777215
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
