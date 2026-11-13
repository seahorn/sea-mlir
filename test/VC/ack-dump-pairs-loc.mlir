// sea-vc --ack-dump-pairs: each lowered read ends with the locations of the
// loads it came from (loc=<l>[;<l>...]; - for none). Two loads at A + i and
// A + j: two reads, one pair; two loads of one cell: one read, both
// locations.
// (--lower-opt=false: cse would merge the two loads of one cell)
// RUN: sea-vc %s --lower --lower-opt=false --seam --ackermannize=on --vcgen-only --ack-dump-pairs=%t
// RUN: FileCheck %s < %t
// CHECK: array 0
// CHECK-DAG: read 0 {{[01]}} {{.*}} loc=k.c:1:1;k.c:3:1
// CHECK-DAG: read 0 {{[01]}} {{.*}} loc=k.c:2:1
// CHECK: pair 0 0 1
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @reads {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %q = sea.ptr.add %A, %j : !sea.ptr<64>, i64
  %x = sea.load %m0[%p] : !m -> i8 loc("k.c":1:1)
  %y = sea.load %m0[%q] : !m -> i8 loc("k.c":2:1)
  %z = sea.load %m0[%p] : !m -> i8 loc("k.c":3:1)
  %s = sea.add %x, %z : i8
  %ok = sea.icmp eq, %s, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
