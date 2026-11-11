// SeaHorn's export form: a check becomes an assume of its failure, then
// `assert false` and halt, so a run in which the check holds is blocked at
// that assume. fuzz.blocked-before-assert-false counts the runs blocked at
// an assume directly followed by assert false (pure ops and unconditional
// branches between them); they stay in fuzz.status as assume-blocked. The
// range assume of x is not such an assume. With --defer-memory-errors,
// deferred= counts the runs among them that went past a memory-safety
// violation first (@late: an out-of-bounds load feeds the check): these
// did not pass a check.
// RUN: sea-run %s --program=main --fuzz=500 --seed=1 --keep-going | FileCheck %s
// RUN: sea-run %s --program=main --fuzz=500 --seed=1 --keep-going --format=jsonl | FileCheck %s --check-prefix=JSONL
// RUN: sea-run %s --program=late --fuzz=500 --seed=1 --keep-going --defer-memory-errors | FileCheck %s --check-prefix=DEFER

// CHECK: fuzz.status: halted=0 assert-failed={{[1-9][0-9]*}} assume-blocked={{[1-9][0-9]*}}
// CHECK: fuzz.blocked-before-assert-false: {{[1-9][0-9]*}} assumes=1 deferred=0{{$}}
// JSONL: {"kind":"stat","key":"fuzz.blocked-before-assert-false","value":{{[1-9][0-9]*}},"assumes":1,"deferred":0,
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

// DEFER: fuzz.deferred: [[D:[1-9][0-9]*]] memory-error=0 assume-blocked=[[D]]
// DEFER: fuzz.blocked-before-assert-false: {{[1-9][0-9]*}} assumes=1 deferred=[[D]]{{$}}
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @late {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "nondet_u64" [2] : i64
  %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
  %v = sea.load %m0[%q] : !pm -> i8
  %c = sea.const 3 : i8
  %err = sea.icmp eq, %v, %c : i8
  sea.assume %err
  %f = sea.const false
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
