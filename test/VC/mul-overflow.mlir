// RUN: sea-vc %s | FileCheck %s
// RUN: %if yices %{ not sea-vc %s --solver=yices 2>&1 | FileCheck %s --check-prefix=YICES %}
// RUN: %if yices %{ sea-vc %s --solver=yices --lower | FileCheck %s %}

// Mul overflow is native for z3 (no lowering needed); Yices needs the
// expansion. The assertion "no signed overflow" fails for some x, y.
// CHECK: result: sat
// CHECK: replay: assert-failed
// YICES: vc.intrinsic
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
