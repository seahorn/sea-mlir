// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=WRAP
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-wrap-flags{mode=assume}))' | sea-vc --lower - | FileCheck %s --check-prefix=ASSUME
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-wrap-flags{mode=assert}))' | FileCheck %s --check-prefix=ASSERT

// nsw on sea.abs is LLVM's is_int_min_poison: a promise that x is not the
// minimal signed value. The VC ignores it (the value wraps), so
// "abs(x) >=s 0" fails at INT_MIN; assumed by sea-wrap-flags, it holds.
// WRAP: result: sat
// WRAP: replay: assert-failed
// ASSUME: result: unsat
// mode=assert checks it with the negation's overflow bit.
// ASSERT: sea.nego
// ASSERT: sea.assert
// ASSERT-NOT: nsw
sea.program @main {
  %x = sea.nondet "nondet_i8" [0] : i8
  %z = sea.const 0 : i8
  %a = sea.abs %x nsw : i8
  %c = sea.icmp sge, %a, %z : i8
  sea.assert %c
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
