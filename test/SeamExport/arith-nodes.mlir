// sea-seam-export: the nodes ne, ugt, uge, sgt, sge (bool), udiv, sdiv,
// urem, srem, bvand, bvor, bvxor, shl, lshr, ashr (bit-vector, operands in
// order), one program using each.
// RUN: sea-seam-export %s --lane=raw | %python %S/Inputs/show.py | FileCheck %s
// CHECK: root %{{[0-9]+}} sea.icmp bv: ite(ne(nd.1_0, nd.2_0), 1, 0)
// CHECK: root %{{[0-9]+}} sea.icmp bv: ite(ugt(nd.1_0, nd.2_0), 1, 0)
// CHECK: root %{{[0-9]+}} sea.icmp bv: ite(uge(nd.1_0, nd.2_0), 1, 0)
// CHECK: root %{{[0-9]+}} sea.icmp bv: ite(sgt(nd.1_0, nd.2_0), 1, 0)
// CHECK: root %{{[0-9]+}} sea.icmp bv: ite(sge(nd.1_0, nd.2_0), 1, 0)
// CHECK: root %{{[0-9]+}} sea.udiv bv: udiv(nd.1_0, nd.2_0)
// CHECK: root %{{[0-9]+}} sea.sdiv bv: sdiv(nd.1_0, nd.2_0)
// CHECK: root %{{[0-9]+}} sea.urem bv: urem(nd.1_0, nd.2_0)
// CHECK: root %{{[0-9]+}} sea.srem bv: srem(nd.1_0, nd.2_0)
// CHECK: root %{{[0-9]+}} sea.and bv: bvand(nd.1_0, nd.2_0)
// CHECK: root %{{[0-9]+}} sea.or bv: bvor(nd.1_0, nd.2_0)
// CHECK: root %{{[0-9]+}} sea.xor bv: bvxor(nd.1_0, nd.2_0)
// CHECK: root %{{[0-9]+}} sea.shl bv: shl(nd.1_0, nd.2_0)
// CHECK: root %{{[0-9]+}} sea.lshr bv: lshr(nd.1_0, nd.2_0)
// CHECK: root %{{[0-9]+}} sea.ashr bv: ashr(nd.1_0, nd.2_0)
// The JSON sorts: the compares are bool, the operators bit-vectors of 8 bits.
// RUN: sea-seam-export %s --lane=raw | %python -I %S/Inputs/sorts.py | FileCheck %s --check-prefix=SORT
// SORT: ne bool None
// SORT: ugt bool None
// SORT: uge bool None
// SORT: sgt bool None
// SORT: sge bool None
// SORT: udiv bv 8
// SORT: sdiv bv 8
// SORT: urem bv 8
// SORT: srem bv 8
// SORT: bvand bv 8
// SORT: bvor bv 8
// SORT: bvxor bv 8
// SORT: shl bv 8
// SORT: lshr bv 8
// SORT: ashr bv 8
sea.program @ops {
  %x = sea.nondet "x" [1] : i8
  %y = sea.nondet "y" [2] : i8
  %c0 = sea.icmp ne, %x, %y : i8
  %c1 = sea.icmp ugt, %x, %y : i8
  %c2 = sea.icmp uge, %x, %y : i8
  %c3 = sea.icmp sgt, %x, %y : i8
  %c4 = sea.icmp sge, %x, %y : i8
  %d0 = sea.udiv %x, %y : i8
  %d1 = sea.sdiv %x, %y : i8
  %r0 = sea.urem %x, %y : i8
  %r1 = sea.srem %x, %y : i8
  %b0 = sea.and %x, %y : i8
  %b1 = sea.or %x, %y : i8
  %b2 = sea.xor %x, %y : i8
  %s0 = sea.shl %x, %y : i8
  %s1 = sea.lshr %x, %y : i8
  %s2 = sea.ashr %x, %y : i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
