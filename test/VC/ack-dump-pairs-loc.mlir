// sea-vc --ack-dump-pairs: each lowered read ends with the locations of the
// loads it came from (loc=<l>[;<l>...]; - for none). Two loads at A + i and
// A + j: two reads, one pair; two loads of one cell: one read, both
// locations.
// (--lower-opt=false: cse would merge the two loads of one cell)
// RUN: sea-vc %s --lower --lower-opt=false --seam --ackermannize=on --vcgen-only --ack-dump-pairs=%t --program=reads
// RUN: FileCheck %s < %t
// CHECK: array 0
// CHECK-DAG: read 0 {{[01]}} {{.*}} loc={{k.c:1:1;k.c:3:1|k.c:3:1;k.c:1:1}}{{$}}
// CHECK-DAG: read 0 {{[01]}} {{.*}} loc=k.c:2:1{{$}}
// CHECK: pair 0 0 1
// @fwd: the load at k.c:9:1 reads back B, where a value computed from the
// load at k.c:1:1 was stored: it reads no array, so no read has its location.
// (A forwarded value that is a bare select would still get both: the gap of
// the read-structure walk, OpSem noteOrigins.)
// RUN: sea-vc %s --program=fwd --lower --seam --ackermannize=on --vcgen-only --ack-dump-pairs=%t.fwd
// RUN: FileCheck %s --check-prefix=FWD < %t.fwd
// FWD-NOT: k.c:9:1
// FWD: read 0 {{[01]}} {{.*}} loc=k.c:1:1{{$}}
// FWD-NOT: k.c:9:1
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
sea.program @fwd {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [6] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %q = sea.ptr.add %A, %j : !sea.ptr<64>, i64
  %x = sea.load %m0[%p] : !m -> i8 loc("k.c":1:1)
  %k1 = sea.const 1 : i8
  %x1 = sea.add %x, %k1 : i8
  %m1 = sea.store %m0[%B], %x1 inplace : !m, i8 loc("k.c":7:1)
  %z = sea.load %m1[%B] : !m -> i8 loc("k.c":9:1)
  %y = sea.load %m0[%q] : !m -> i8 loc("k.c":2:1)
  %ok = sea.icmp eq, %z, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
