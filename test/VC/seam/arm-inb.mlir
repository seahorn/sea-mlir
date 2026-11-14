// Out-of-object under ARM: the store address q = p - 16 is a sum over an ite
// with five leaves; for p = o5 it is the base of e: the access leaves its
// object o5 (the objects are adjacent: o5 is 16 bytes above e) and the store
// overwrites e. The plain VC is sat (the assertion fails). An access whose
// ARM entry lies wholly outside its object is a static out-of-object access
// (as a numeral offset is in the view case): reported, where it lands
// unknown (no numeral offset), and the premises stay. ARM decides q != e
// from the bases (A-SEP, which this store breaks): unsat, conditional on a
// broken premise.
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s --check-prefix=STATIC
// RUN: sea-vc %s --lower --seam --seam-premises=0 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --seam --seam-arm=0 | FileCheck %s --check-prefix=SAT
// STATIC: [vc.seam-out-of-object]
// STATIC: seam.out-of-object-at: {{.*}} store: {{.*}}(ARM) lies outside its object lands=unknown
// STATIC: seam.premises: assumed
// STATIC: seam.out-of-object: accesses=1 object=0 shared=0 unknown=1
// STATIC: result: unsat
// SAT: result: sat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @armInb {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %o1 = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %o2 = sea.alloca %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %o3 = sea.alloca %c16 align 8 [3] : i64 -> !sea.ptr<64>
  %o4 = sea.alloca %c16 align 8 [4] : i64 -> !sea.ptr<64>
  %o5 = sea.alloca %c16 align 8 [5] : i64 -> !sea.ptr<64>
  %e = sea.alloca %c16 align 8 [6] : i64 -> !sea.ptr<64>
  %b1 = sea.nondet "b1" [11] : i1
  %b2 = sea.nondet "b2" [12] : i1
  %b3 = sea.nondet "b3" [13] : i1
  %b4 = sea.nondet "b4" [14] : i1
  %p4 = sea.select %b4, %o4, %o5 : !sea.ptr<64>
  %p3 = sea.select %b3, %o3, %p4 : !sea.ptr<64>
  %p2 = sea.select %b2, %o2, %p3 : !sea.ptr<64>
  %p = sea.select %b1, %o1, %p2 : !sea.ptr<64>
  %cm16 = sea.const -16 : i64
  %q = sea.ptr.add %p, %cm16 : !sea.ptr<64>, i64
  %v1 = sea.const 1 : i8
  %v9 = sea.const 9 : i8
  %m1 = sea.store %m0[%e], %v1 inplace : !m, i8
  %m2 = sea.store %m1[%q], %v9 inplace : !m, i8
  %x = sea.load %m2[%e] : !m -> i8
  %ok = sea.icmp eq, %x, %v1 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
