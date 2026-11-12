// ptrindex.absorb-k: a pointer X stored at ite(c, f, u) and loaded at f in
// byte mode, f = P + 16i a promised (inbounds) node, u = P + 8 an
// unflagged one, P a loaded pointer. With --seam-pull-index byte k of the
// store address, PTR_INDEX(ite(c, f, u), c = k), is pushed into the ite:
// its then leaf is the unflagged PTR_INDEX(f, c = k), while byte k of the
// load is f with K = k (op::ptr::addressPlus). absorb-k rewrites the first
// into the second, so byte k of one address is one term and its alias
// questions with the load's bytes are the ones the load asks: 15 open
// questions (21 without the rule: the two forms cancel to different terms).
// RUN: sea-vc %s --lower --seam --seam-pull-index --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-pull-index --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK: seam.alias.open.all: 15
// CHECK: seam.rule.ptrindex.absorb-k: {{[1-9]}}
// CHECK: result: unsat
// CHK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @absorb_k {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c8 = sea.const 8 : i64
  %H = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %X = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %P = sea.load %m0[%H] : !m -> !sea.ptr<64>
  %i = sea.nondet "i" [3] : i64
  %c = sea.nondet "c" [6] : i1
  %f = sea.ptr.index %P [%i] strides [16] offset 0 inbounds : !sea.ptr<64>, i64
  %u = sea.ptr.add %P, %c8 : !sea.ptr<64>, i64
  %a = sea.select %c, %f, %u : !sea.ptr<64>
  %m1 = sea.store %m0[%a], %X inplace : !m, !sea.ptr<64>
  %r = sea.load %m1[%f] : !m -> !sea.ptr<64>
  // c: the store was at f, the load reads X
  %eq = sea.ptr.cmp eq, %r, %X : !sea.ptr<64>
  %t1 = sea.const 1 : i1
  %nc = sea.xor %c, %t1 : i1
  %ok = sea.or %eq, %nc : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
