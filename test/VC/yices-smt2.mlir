// REQUIRES: yices
// Yices has no SMT-LIB printer: --smt2 warns instead of printing.
// RUN: sea-vc %s --program=safe --solver=yices --smt2 2>&1 | FileCheck %s
// CHECK: [yices.smt2-unimplemented]
sea.program @safe {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c0 = sea.const 0 : i8
  %ok = sea.icmp eq, %x, %x : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
