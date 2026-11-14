// RUN: not sea-vc %s 2>&1 | FileCheck %s --check-prefix=RAW
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=SAT

// The VC refuses sea.abs; --lower expands it to ite(x <s 0, 0 - x, x).
// RAW: vc.intrinsic
// "abs(x) >=s 0" fails exactly at the minimal signed value: sat, and the
// replay confirms the interpreter's value.
// SAT: result: sat
// SAT: replay: assert-failed
// SAT: mismatches=0
sea.program @main {
  %x = sea.nondet "nondet_i8" [0] : i8
  %z = sea.const 0 : i8
  %a = sea.abs %x : i8
  %c = sea.icmp sge, %a, %z : i8
  sea.assert %c
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
