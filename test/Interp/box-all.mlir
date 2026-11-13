// RUN: sea-run %s --program=zeros | FileCheck %s --check-prefix=ZEROS
// RUN: sea-run %s --program=zeros_bug | FileCheck %s --check-prefix=ZEROS-BUG
// RUN: sea-run %s --program=empty --input=0=0 | FileCheck %s --check-prefix=HALT
// RUN: sea-run %s --program=empty --input=0=1 | FileCheck %s --check-prefix=EMPTY-1
// RUN: sea-run %s --program=step --input=0=9 | FileCheck %s --check-prefix=STEP-9
// RUN: sea-run %s --program=step --input=0=4 | FileCheck %s --check-prefix=HALT
// RUN: sea-run %s --program=step --input=0=11 | FileCheck %s --check-prefix=HALT
// RUN: sea-run %s --program=two --input=0=5 | FileCheck %s --check-prefix=TWO-5
// RUN: sea-run %s --program=two --input=0=6 | FileCheck %s --check-prefix=HALT
// RUN: sea-run %s --program=huge --max-steps=1000 | FileCheck %s --check-prefix=HUGE
// RUN: sea-run %s --program=huge_empty_body --max-steps=1000 | FileCheck %s --check-prefix=HUGE
// The interpreter evaluates a sea.box.all over its whole box
// (docs/design/BoxAll.md): unsigned bounds, upper bound exclusive, an empty
// box is true; a failing run names the first false index.

// Every cell of a zero-filled object is 0; one store of 1 at offset 3 makes
// index 3 the first false one.
// ZEROS: status: halted
// ZEROS-BUG: status: assert-failed
// ZEROS-BUG: box-all-false: index=(3)
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @zeros {
  %c8 = sea.const 8 : i64
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !m
  %a = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %all = sea.box.all (%i) = (0) to (%c8) {
    %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
    %x = sea.load %m[%p] : !m -> i8
    %e = sea.icmp eq, %x, %z : i8
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

sea.program @zeros_bug {
  %c3 = sea.const 3 : i64
  %c8 = sea.const 8 : i64
  %z = sea.const 0 : i8
  %one = sea.const 1 : i8
  %m = sea.mem.const %z : !m
  %a = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %p3 = sea.ptr.add %a, %c3 : !sea.ptr<64>, i64
  %m1 = sea.store %m[%p3], %one inplace : !m, i8
  %all = sea.box.all (%i) = (0) to (%c8) {
    %p = sea.ptr.add %a, %i : !sea.ptr<64>, i64
    %x = sea.load %m1[%p] : !m -> i8
    %e = sea.icmp eq, %x, %z : i8
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// A body that is false everywhere: true only on the empty box.
// HALT: status: halted
// EMPTY-1: status: assert-failed
// EMPTY-1: box-all-false: index=(0)
sea.program @empty {
  %n = sea.nondet "n" [0] : i64
  %all = sea.box.all (%i) = (0) to (%n) {
    %f = sea.const 0 : i1
    sea.yield %f
  }
  sea.assert %all
  sea.halt
}

// Indices 3, 5, 7, 9 (lower bound 3, step 2, upper bound 10, exclusive).
// STEP-9: status: assert-failed
// STEP-9: box-all-false: index=(9)
sea.program @step {
  %x = sea.nondet "x" [0] : i64
  %c10 = sea.const 10 : i64
  %all = sea.box.all (%k) = (3) to (%c10) step (2) {
    %e = sea.icmp ne, %k, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// (i, j) in [0, 2) x [0, 3): 3i + j covers 0..5.
// TWO-5: status: assert-failed
// TWO-5: box-all-false: index=(1, 2)
sea.program @two {
  %x = sea.nondet "x" [0] : i64
  %c2 = sea.const 2 : i64
  %c3 = sea.const 3 : i64
  %all = sea.box.all (%i, %j) = (0, 0) to (%c2, %c3) {
    %r = sea.mul %i, %c3 : i64
    %v = sea.add %r, %j : i64
    %e = sea.icmp ne, %v, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// Every body op is a step: a box too large to run stops at the step limit.
// HUGE: status: step-limit
sea.program @huge {
  %c = sea.const -1 : i64
  %all = sea.box.all (%i) = (0) to (%c) {
    %t = sea.const 1 : i1
    sea.yield %t
  }
  sea.assert %all
  sea.halt
}

// A body without ops (a yield of an outer value, as cse can leave): each
// index is a step of its own.
sea.program @huge_empty_body {
  %c = sea.const -1 : i64
  %t = sea.const 1 : i1
  %all = sea.box.all (%i) = (0) to (%c) {
    sea.yield %t
  }
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
