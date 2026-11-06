// RUN: sea-opt %s --sea-sink-assumes | FileCheck %s
// RUN: sea-vc %s --program=diamond --sink-assumes | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=diamond | FileCheck %s --check-prefix=SAT0
// RUN: sea-vc %s --program=entry_and_arm --sink-assumes | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=entry_and_arm | FileCheck %s --check-prefix=UNSAT0
// RUN: sea-vc %s --program=load --lower --sink-assumes | FileCheck %s --check-prefix=LOAD
// RUN: sea-vc %s --program=load --lower | FileCheck %s --check-prefix=LOAD0
// RUN: sea-vc %s --program=none --sink-assumes | FileCheck %s --check-prefix=NONE

// An assume on one arm of a diamond: the join gets an i1 argument, the arm
// passes its condition, the other arm true; one assume before the assert.
// CHECK-LABEL: sea.program @diamond
// CHECK-NOT: sea.assume
// CHECK: sea.cond_br %{{.*}}, ^[[T:bb[0-9]+]], ^[[E:bb[0-9]+]]
// CHECK: ^[[T]]:
// CHECK-NOT: sea.assume
// CHECK: sea.br ^[[J:bb[0-9]+]](%[[NZ:[0-9]+]] : i1)
// CHECK: ^[[E]]:
// CHECK: %[[TRUE:.*]] = sea.const true
// CHECK-NEXT: sea.br ^[[J]](%[[TRUE]] : i1)
// CHECK: ^[[J]](%[[ACC:.*]]: i1):
// CHECK: sea.assume %[[ACC]]
// CHECK-NEXT: sea.assert
// The assume on the arm not taken leaves the other arm's bug.
// SAT: vc.sink-assumes: 1
// SAT: result: sat
// SAT: validate: checked={{[0-9]+}} mismatches=0
// SAT0: result: sat
sea.program @diamond {
  %x = sea.nondet "nondet_u8" [0] : i8
  %b = sea.nondet "nondet_bool" [1] : i1
  sea.cond_br %b, ^then, ^else
^then:
  %c0 = sea.const 0 : i8
  %nz = sea.icmp ne, %x, %c0 : i8
  sea.assume %nz
  sea.br ^join
^else:
  sea.br ^join
^join:
  %c0b = sea.const 0 : i8
  %ok = sea.icmp ne, %x, %c0b : i8
  sea.assert %ok
  sea.halt
}

// Assumes in the entry and on both arms: the conjunction on each path
// (sea.and of the entry's condition and the arm's).
// CHECK-LABEL: sea.program @entry_and_arm
// CHECK: %[[LT:.*]] = sea.icmp ult
// CHECK-NOT: sea.assume
// CHECK: sea.and %[[LT]], %{{.*}} : i1
// CHECK: sea.and %[[LT]], %{{.*}} : i1
// CHECK: sea.assume
// CHECK-NEXT: sea.assert
// UNSAT: vc.sink-assumes: 3
// UNSAT: result: unsat
// UNSAT0: result: unsat
sea.program @entry_and_arm {
  %x = sea.nondet "nondet_u8" [0] : i8
  %b = sea.nondet "nondet_bool" [1] : i1
  %c100 = sea.const 100 : i8
  %lt = sea.icmp ult, %x, %c100 : i8
  sea.assume %lt
  sea.cond_br %b, ^then, ^else
^then:
  %c1 = sea.const 1 : i8
  %ge = sea.icmp uge, %x, %c1 : i8
  sea.assume %ge
  sea.br ^join
^else:
  %c2 = sea.const 2 : i8
  %ge2 = sea.icmp uge, %x, %c2 : i8
  sea.assume %ge2
  sea.br ^join
^join:
  %c0 = sea.const 0 : i8
  %ok = sea.icmp ne, %x, %c0 : i8
  sea.assert %ok
  sea.halt
}

// The condition reads memory before a store to the same cell: the value
// at the assume is kept (an SSA value), not the cell's value at the end.
// LOAD: vc.sink-assumes: 1
// LOAD: result: unsat
// LOAD0: result: unsat
sea.program @load {
  %m = sea.mem.nondet "region" [0] : !sea.mem<!sea.ptr<64>, i8>
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 4 [1] : i64 -> !sea.ptr<64>
  %v = sea.load %m[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  %c0 = sea.const 0 : i8
  %z = sea.icmp eq, %v, %c0 : i8
  sea.assume %z
  %c7 = sea.const 7 : i8
  %m2 = sea.store %m[%p], %c7 inplace : !sea.mem<!sea.ptr<64>, i8>, i8
  %w = sea.load %m2[%p] : !sea.mem<!sea.ptr<64>, i8> -> i8
  %seven = sea.icmp eq, %w, %c7 : i8
  %zero = sea.icmp eq, %v, %c0 : i8
  %ok = sea.and %seven, %zero : i1
  sea.assert %ok
  sea.halt
}

// No assume: nothing to do.
// NONE: vc.sink-assumes: 0
// NONE: result: sat
sea.program @none {
  %f = sea.const false
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
