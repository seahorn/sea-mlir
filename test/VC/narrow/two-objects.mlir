// --narrow-offsets narrows each read into its own object, but a pair of
// reads of two different objects is not compared at k bits: it stays a
// 64-bit address comparison (vc.narrow-mixed), over base + zext(low_k).
// --ack-decider=false: the difference decider drops these pairs (their
// offsets are bounded); the test needs them constrained.
// RUN: sea-vc %s --lower --seam --ack-decider=false --narrow-offsets 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --ack-decider=false --narrow-offsets --smt2 2>/dev/null | FileCheck %s --check-prefix=SMT
// CHECK: vc.narrow-reads: 2
// CHECK: vc.narrow-objects: 2
// CHECK: vc.narrow-pairs: 0
// CHECK: vc.narrow-mixed: 1
// CHECK: result: sat
// CHECK: validate: checked={{[0-9]+}} mismatches=0
// SMT: (bvadd obj.{{.*}} ((_ zero_extend {{[0-9]+}})
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @two {
  %m = sea.mem.nondet "m" [0] : !m
  %n = sea.const 64 : i64
  %a = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %n align 8 [2] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [3] : i64
  %j = sea.nondet "j" [4] : i64
  %ilt = sea.icmp ult, %i, %n : i64
  sea.assume %ilt
  %jlt = sea.icmp ult, %j, %n : i64
  sea.assume %jlt
  %pi = sea.ptr.add %a, %i : !sea.ptr<64>, i64
  %pj = sea.ptr.add %b, %j : !sea.ptr<64>, i64
  %x = sea.load %m[%pi] : !m -> i8
  %y = sea.load %m[%pj] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
