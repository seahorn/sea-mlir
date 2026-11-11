// SeaHorn's export form: a check becomes an assume of its failure, then
// `assert false` and halt, so a run in which the check holds is blocked at
// that assume. fuzz.blocked-before-assert-false counts the runs blocked at
// an assume directly followed by assert false (pure ops and unconditional
// branches between them); they stay in fuzz.status as assume-blocked. The
// range assume of x is not such an assume.
// RUN: sea-run %s --fuzz=500 --seed=1 --keep-going | FileCheck %s
// RUN: sea-run %s --fuzz=500 --seed=1 --keep-going --format=jsonl | FileCheck %s --check-prefix=JSONL

// CHECK: fuzz.status: halted=0 assert-failed={{[1-9][0-9]*}} assume-blocked={{[1-9][0-9]*}}
// CHECK: fuzz.blocked-before-assert-false: {{[1-9][0-9]*}} assumes=1{{$}}
// JSONL: {"kind":"stat","key":"fuzz.blocked-before-assert-false","value":{{[1-9][0-9]*}},"assumes":1,
sea.program @main {
  %x = sea.nondet "nd_u8" [0] : i8
  %c10 = sea.const 10 : i8
  %in = sea.icmp ult, %x, %c10 : i8
  sea.assume %in
  // the check: x != 9; its failure is assumed
  %c9 = sea.const 9 : i8
  %ok = sea.icmp ne, %x, %c9 : i8
  %t = sea.const true
  %err = sea.xor %ok, %t : i1
  sea.assume %err
  sea.br ^fail
^fail:
  %f = sea.const false
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
