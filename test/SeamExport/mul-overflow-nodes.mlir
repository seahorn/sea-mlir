// sea-seam-export: sea.umulo / sea.smulo as sea-vc encodes them for its
// default solver (z3): the native predicates umul_no_overflow,
// smul_no_overflow, smul_no_underflow (bool, two bit-vector operands of one
// width; bv::mulOverflows), not the vc.intrinsic error.
// RUN: sea-seam-export %s --lane=raw | %python %S/Inputs/show.py | FileCheck %s --check-prefix=RAW
// RUN: sea-seam-export %s --lane=simplify | %python %S/Inputs/show.py | FileCheck %s --check-prefix=SIMP
// RAW: root %{{[0-9]+}} sea.umulo bv: ite(not(umul_no_overflow(nd.1_0, nd.2_0)), 1, 0)
// RAW: root %{{[0-9]+}} sea.smulo bv: ite(not(and(smul_no_overflow(nd.1_0, nd.2_0), smul_no_underflow(nd.1_0, nd.2_0))), 1, 0)
// RAW: root %{{[0-9]+}} sea.umulo bv: ite(not(umul_no_overflow(nd.3_0, nd.4_0)), 1, 0)
// RAW: root %{{[0-9]+}} sea.smulo bv: ite(not(and(smul_no_overflow(nd.3_0, nd.4_0), smul_no_underflow(nd.3_0, nd.4_0))), 1, 0)
// The simplify lane keeps the predicates (the negation goes into the ite).
// SIMP: root %{{[0-9]+}} sea.umulo bv: ite(umul_no_overflow(nd.1_0, nd.2_0), 0, 1)
// SIMP: root %{{[0-9]+}} sea.smulo bv: ite(and(smul_no_overflow(nd.1_0, nd.2_0), smul_no_underflow(nd.1_0, nd.2_0)), 0, 1)
// The JSON sorts: each node is a bool term over two bit-vector operands of
// the op's width (i8, then i16).
// RUN: sea-seam-export %s --lane=raw | %python -I %S/Inputs/node-args.py umul_no_overflow smul_no_overflow smul_no_underflow | FileCheck %s --check-prefix=SORT
// SORT:      umul_no_overflow bool: bv/8 bv/8
// SORT-NEXT: smul_no_overflow bool: bv/8 bv/8
// SORT-NEXT: smul_no_underflow bool: bv/8 bv/8
// SORT-NEXT: umul_no_overflow bool: bv/16 bv/16
// SORT-NEXT: smul_no_overflow bool: bv/16 bv/16
// SORT-NEXT: smul_no_underflow bool: bv/16 bv/16
sea.program @mulo {
  %x = sea.nondet "x" [1] : i8
  %y = sea.nondet "y" [2] : i8
  %u = sea.umulo %x, %y : i8
  %s = sea.smulo %x, %y : i8
  %a = sea.nondet "a" [3] : i16
  %b = sea.nondet "b" [4] : i16
  %u2 = sea.umulo %a, %b : i16
  %s2 = sea.smulo %a, %b : i16
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
