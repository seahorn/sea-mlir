// RUN: sea-vc %s --seam --seam-stats --seam-stride --seam-check | FileCheck %s

// The --seam-check premise of an R-STRIDE firing at an ite of promised
// nodes (docs/design/PtrIndex.md sections 5 and 8): on the path where a
// leaf is selected only its promise is a fact, so the premise is
// ite(c, P(N1), P(N2)), not P(N1) /\ P(N2). Here the read is at
// select(c, N1, N2), c = i <u 43, N1 = a[3 i], N2 = a[3 j], j = i + 100
// (nusw): c -> 3 j does not fit 8 signed bits, !c -> 3 i does not, so the
// conjunction is false in every state (a vacuous premise, not counted as
// valid) while the guarded premise holds where the selected access is
// defined. Both entries (a + 50, a + 100) are stride-apart from both
// leaves; each firing is checked and valid.
// CHECK: seam.rule.row.smap-abort-stride: 2
// CHECK: result: unsat
// CHECK-NOT: vacuous premise
// CHECK: seam-check: firings={{[0-9]+}} checked={{[0-9]+}} valid={{[0-9]+}} invalid=0 unknown=0 unexpressible=0
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @vac {
  %a = sea.segment "heap.1" base 1 : i8 size 200 : i8 align 1 kind "heap" : !sea.addr<8>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %v7 = sea.const 7 : i8
  %o = sea.const 50 : i8
  %p = sea.addr.add %a, %o : !sea.addr<8>, i8
  %m1a = sea.store %m0[%p], %v7 inplace : !am, i8
  %o2 = sea.const 100 : i8
  %p2 = sea.addr.add %a, %o2 : !sea.addr<8>, i8
  %m1 = sea.store %m1a[%p2], %v7 inplace : !am, i8
  %i = sea.nondet "i" [1] : i8
  %h = sea.const 100 : i8
  %j = sea.add %i, %h : i8
  %b43 = sea.const 43 : i8
  %c = sea.icmp ult, %i, %b43 : i8
  %n1 = sea.ptr.index %a [%i] strides [3] offset 0 nusw : !sea.addr<8>, i8
  %n2 = sea.ptr.index %a [%j] strides [3] offset 0 nusw : !sea.addr<8>, i8
  %q = sea.select %c, %n1, %n2 : !sea.addr<8>
  %x = sea.load %m1[%q] : !am -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
