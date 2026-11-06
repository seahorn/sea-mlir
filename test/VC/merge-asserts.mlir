// sea-merge-asserts (multi-assert support; ctac ua merge): any number of
// sea.assert ops become one exit block with the only assert and halt. @two:
// the first assert holds, the second fails for x = 5: sat, replayed (the
// interpreter fails the original assert). @hold: both hold: unsat.
// RUN: sea-vc %s --program=two --lower | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=hold --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-merge-asserts))' | FileCheck %s --check-prefix=IR
// SAT: result: sat
// SAT: replay: assert-failed
// UNSAT: result: unsat
// IR-LABEL: sea.program @two
// IR-COUNT-1: sea.assert
// IR-COUNT-2: sea.assume
// IR: sea.halt
sea.program @two {
  %x = sea.nondet "x" [0] : i8
  %c10 = sea.const 10 : i8
  %c5 = sea.const 5 : i8
  %lt = sea.icmp ult, %x, %c10 : i8
  sea.assume %lt
  %a = sea.icmp ult, %x, %c10 : i8
  sea.assert %a
  %b = sea.icmp ne, %x, %c5 : i8
  sea.assert %b
  sea.halt
}
sea.program @hold {
  %x = sea.nondet "x" [0] : i8
  %c10 = sea.const 10 : i8
  %lt = sea.icmp ult, %x, %c10 : i8
  sea.assume %lt
  %a = sea.icmp ult, %x, %c10 : i8
  sea.assert %a
  %c20 = sea.const 20 : i8
  %b = sea.icmp ult, %x, %c20 : i8
  sea.assert %b
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
