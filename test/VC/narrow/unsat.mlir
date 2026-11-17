// --narrow-offsets on an unsat program: two reads of one 1232-byte object
// (k = 11) at symbolic in-bounds offsets i and j; i = j must give equal
// bytes. The pair is compared at 11 bits and the answer stays unsat.
// RUN: sea-vc %s --lower --seam --narrow-offsets 2>&1 | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --seam --narrow-offsets --solver=yices 2>&1 | FileCheck %s %}
// CHECK: vc.narrow-reads: 2
// CHECK: vc.narrow-k-max: 11
// CHECK: vc.narrow-pairs: 1
// CHECK: assume.a-inb: narrow-reads=2 narrow-pairs=1
// CHECK: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @same {
  %m = sea.mem.nondet "m" [0] : !m
  %n = sea.const 1232 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %ilt = sea.icmp ult, %i, %n : i64
  sea.assume %ilt
  %jlt = sea.icmp ult, %j, %n : i64
  sea.assume %jlt
  %pi = sea.ptr.add %p, %i : !sea.ptr<64>, i64
  %pj = sea.ptr.add %p, %j : !sea.ptr<64>, i64
  %x = sea.load %m[%pi] : !m -> i8
  %y = sea.load %m[%pj] : !m -> i8
  %eqij = sea.icmp eq, %i, %j : i64
  %eqxy = sea.icmp eq, %x, %y : i8
  %t = sea.const 1 : i1
  %neq = sea.xor %eqij, %t : i1
  %ok = sea.or %neq, %eqxy : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
