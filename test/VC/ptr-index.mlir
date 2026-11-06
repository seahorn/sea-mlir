// RUN: sea-vc %s --program=unsat --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=unsat --lower --seam | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=sat --lower | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=sat --lower --seam | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=raw | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=raw --seam | FileCheck %s --check-prefix=UNSAT

// sea.ptr.index through the VC (docs/design/PtrIndex.md:
// the VC encodes the op, a PTR_INDEX node with --seam, the lowering's
// arithmetic without). @unsat:
// p[i].f with stride 8 and field 4, and p + 8i + 4 meet. @sat: a negative
// stride, i = 2: p + 24 - 2*4 = p + 16 is p[2] of stride 8 for i = 2, the
// assert says it is not: sat, replayed. @raw: a placed program, no
// lowering: the op reaches the VC as it is.
// UNSAT: result: unsat
// SAT: result: sat
// SAT: replay: assert-failed
// SAT: validate: checked={{[0-9]+}} mismatches=0
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @unsat {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 64 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %seven = sea.const 7 : i64
  %iok = sea.icmp ult, %i, %seven : i64
  sea.assume %iok
  %f = sea.ptr.index %p [%i] strides [8] offset 4 inbounds : !sea.ptr<64>, i64
  %v = sea.const 9 : i8
  %m1 = sea.store %m0[%f], %v inplace : !pm, i8
  %c8 = sea.const 8 : i64
  %o = sea.mul %i, %c8 : i64
  %c4 = sea.const 4 : i64
  %o4 = sea.add %o, %c4 : i64
  %g = sea.ptr.add %p, %o4 : !sea.ptr<64>, i64
  %x = sea.load %m1[%g] : !pm -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}
sea.program @sat {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 64 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %seven = sea.const 7 : i64
  %iok = sea.icmp ult, %i, %seven : i64
  sea.assume %iok
  %c24 = sea.const 24 : i64
  %p24 = sea.ptr.add %p, %c24 : !sea.ptr<64>, i64
  %f = sea.ptr.index %p24 [%i] strides [-4] offset 0 inbounds : !sea.ptr<64>, i64
  %g = sea.ptr.index %p [%i] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %v = sea.const 9 : i8
  %m1 = sea.store %m0[%f], %v inplace : !pm, i8
  %x = sea.load %m1[%g] : !pm -> i8
  %ok = sea.icmp ne, %x, %v : i8
  sea.assert %ok
  sea.halt
}
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @raw {
  %a = sea.segment "heap.1" base 16 : i8 size 32 : i8 align 8 kind "heap" : !sea.addr<8>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %i = sea.nondet "i" [1] : i8
  %q = sea.ptr.index %a [%i] strides [4] offset 0 inbounds : !sea.addr<8>, i8
  %x = sea.load %m0[%q] : !am -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
