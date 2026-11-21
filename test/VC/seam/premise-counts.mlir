// The premises A-NOWRAP and A-NULL where they fire.
// @kwide: R-STRIDE (--seam-stride) over a 2-byte read at the slot GEP
// p + 24 i + 96 (inbounds) under a store-map of header bytes at 8 and 16:
// byte 1 of the read is the node with K = 1, whose skip rests on A-NOWRAP
// (the access address does not wrap); --seam-check proves each firing.
// cmp.congruence (default on) decides that byte without a premise, so
// @kwide runs with --seam-congruence=false.
// @nul: two reads of the initial memory, one at a node over null (size 0),
// one over p: the Ackermann stride rule drops the pair under A-NULL (no
// access of positive width through null), and the null read is reported
// out of its object.
// RUN: sea-vc %s --program=kwide --lower --seam --seam-congruence=false --seam-stats --seam-stride --seam-check | FileCheck %s --check-prefix=KWIDE
// RUN: sea-vc %s --program=nul --lower --seam --ack-stride 2>&1 | FileCheck %s --check-prefix=NUL
// KWIDE: assume.a-nowrap: seam={{[1-9][0-9]*}}
// KWIDE: seam.rule.premise.a-nowrap: {{[1-9][0-9]*}}
// KWIDE: result: unsat
// KWIDE: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
// NUL: seam.out-of-object-at: {{.*}} load: access of 1 unit(s) in object null of size 0
// NUL: vc.ackermann-stride-a-null-pairs: {{[1-9][0-9]*}}
// NUL: result: sat
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @kwide {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 192 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %c8 = sea.const 8 : i64
  %c16 = sea.const 16 : i64
  %h1 = sea.ptr.add %p, %c8 : !sea.ptr<64>, i64
  %h2 = sea.ptr.add %p, %c16 : !sea.ptr<64>, i64
  %a = sea.const 1 : i8
  %b = sea.const 2 : i8
  %s = sea.ptr.index %p [%i] strides [24] offset 96 inbounds : !sea.ptr<64>, i64
  %y = sea.load %m0[%s] : !pm -> i16
  %m1 = sea.store %m0[%h1], %a inplace : !pm, i8
  %m2 = sea.store %m1[%h2], %b inplace : !pm, i8
  %x = sea.load %m2[%s] : !pm -> i16
  %ok = sea.icmp eq, %x, %y : i16
  sea.assert %ok
  sea.halt
}
sea.program @nul {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 96 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %z = sea.ptr.null : !sea.ptr<64>
  %i = sea.nondet "i" [2] : i64
  %j = sea.nondet "j" [3] : i64
  %a = sea.ptr.index %z [%i] strides [24] offset 8 inbounds : !sea.ptr<64>, i64
  %b = sea.ptr.index %p [%j] strides [24] offset 16 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m0[%a] : !pm -> i8
  %y = sea.load %m0[%b] : !pm -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
