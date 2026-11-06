// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-wrap-flags{mode=assume}))' | FileCheck %s --check-prefix=ASSUME
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-wrap-flags{mode=drop}))' | FileCheck %s --check-prefix=DROP
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-wrap-flags{mode=assume}))' | sea-vc --lower - | FileCheck %s --check-prefix=VC
// RUN: sea-vc %s | FileCheck %s --check-prefix=WRAP

// Pins the mapping of flags to overflow ops and what each mode does with
// it; the pass's agreement with the promises is fuzzed (utils/fuzz-vc.py).
// ASSUME: sea.saddo
// ASSUME: sea.assume
// ASSUME: sea.umulo
// ASSUME: sea.assume
// ASSUME-NOT: nsw
// DROP-NOT: sea.assume
// DROP-NOT: nsw
// x + 1 > x holds when the add is assumed not to overflow (signed); the VC
// ignores the flag, so without the pass the wrap makes it fail.
// VC: result: unsat
// WRAP: result: sat
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %one = sea.const 1 : i8
  %y = sea.add %x, %one nsw : i8
  %z = sea.mul %x, %x nuw : i8
  %c = sea.icmp sgt, %y, %x : i8
  sea.assert %c
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
