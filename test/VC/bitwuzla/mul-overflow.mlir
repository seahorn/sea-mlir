// REQUIRES: bitwuzla
// RUN: sea-vc %s --solver=bitwuzla | FileCheck %s
// RUN: sea-vc %s --solver=bitwuzla --lower | FileCheck %s

// Mul overflow is native for Bitwuzla as for z3 (bvsmulo, split like z3's
// no-overflow / no-underflow; no lowering needed; the semantics are checked
// by check-semantics.py --bitwuzla). The assertion "no signed overflow" fails
// for some x, y.
// CHECK: result: sat
// CHECK: replay: assert-failed
// CHECK: validate: checked={{[0-9]+}} mismatches=0
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %o = sea.smulo %x, %y : i8
  %t = sea.const 1 : i1
  %ok = sea.xor %o, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
