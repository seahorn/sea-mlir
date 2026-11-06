// sea-seam-export, every lane: a sea.ptr.index node (strides, constant,
// byte offset, flag), an address add merged into it, a difference and an
// escape. The raw lanes keep the node as the encoding builds it; the
// rewriting lanes fold its numeral index; the no-SeaM boundary
// (PtrIndexExpander) erases it with the lowering's arithmetic.
// RUN: sea-seam-export %s --address-bits=8 | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,RAW -DLANE=memory-rules0
// RUN: sea-seam-export %s --rewrite | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,REW -DLANE=seam-wide-inb0-simplify0-layout0
// RUN: sea-seam-export %s --lane=raw-wide | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,RAW -DLANE=raw-wide
// RUN: sea-seam-export %s --lane=raw | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,RAW -DLANE=raw
// RUN: sea-seam-export %s --lane=seam-wide-inb0 | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,SEAM -DLANE=seam-wide-inb0
// RUN: sea-seam-export %s --lane=seam-inb0 | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,SEAM -DLANE=seam-inb0
// RUN: sea-seam-export %s --lane=seam-wide | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,SEAM,INB -DLANE=seam-wide
// RUN: sea-seam-export %s --lane=seam | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,SEAMENC,SEAM,INB -DLANE=seam
// RUN: sea-seam-export %s --lane=plain | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,NOSEAM,PLAIN -DLANE=plain
// RUN: sea-seam-export %s --lane=simplify | %python %S/Inputs/show.py | FileCheck %s --allow-unused-prefixes --check-prefixes=ALL,NOSEAM,SIMP -DLANE=simplify
// ALL: header: schema=2 lane=[[LANE]]
// RAW: root %q sea.ptr.index ptr: index(fromBits(nd.0_0), nd.1_0*254, 1*4, c=255, K=0, plain)
// RAW: root %a sea.addr.add ptr: index(fromBits(nd.0_0), nd.1_0*254, 1*4, c=0, K=0, plain)
// RAW: root %d sea.addr.diff bv: diff(index(
// REW: root %q sea.ptr.index ptr: index(fromBits(nd.0_0), nd.1_0*254, 1*4, c=255, K=0, plain)
// PLAIN: root %q sea.ptr.index ptr: index(fromBits(nd.0_0), nd.1_0*254, 1*4, c=255, K=0, plain)
// SEAM: root %q sea.ptr.index ptr: index(fromBits(nd.0_0), nd.1_0*254, c=3, K=0, plain)
// SEAM: root %d sea.addr.diff bv: sub(addr(index(fromBits(nd.0_0), nd.1_0*254, c=4, K=0, plain)), addr(fromBits(nd.0_0)))
// SIMP: root %q sea.ptr.index ptr: index(fromBits(nd.0_0), nd.1_0*254, c=3, K=0, plain)
// ALL: root %v sea.addr.to_int bv: toInt(index(
// SeaM's boundary: one n-ary sum; the plain one: nested adds.
// RAW: erased %q: add(nd.0_0, mul(nd.1_0, 254), mul(1, 4), 255)
// SEAM: erased %q: add(nd.0_0, mul(nd.1_0, 254), 3)
// PLAIN: erased %q: add(nd.0_0, add(add(mul(nd.1_0, 254), mul(1, 4)), 255))
// SIMP: erased %q: add(nd.0_0, mul(nd.1_0, 254), 3)
// ALL: erased %d: sub(
sea.program @main {
  %raw = sea.nondet "p" [0] : i8 loc("raw")
  %p = sea.addr.from_bits %raw : i8 to !sea.addr<8> loc("p")
  %i = sea.nondet "i" [1] : i8 loc("i")
  %one = sea.const 1 : i8 loc("one")
  %q = sea.ptr.index %p [%i, %one] strides [-2, 4] offset -1 : !sea.addr<8>, i8, i8 loc("q")
  %a = sea.addr.add %q, %one : !sea.addr<8>, i8 loc("a")
  %d = sea.addr.diff %a, %p : !sea.addr<8> -> i8 loc("d")
  %v = sea.addr.to_int %q : !sea.addr<8> to i8 loc("v")
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
