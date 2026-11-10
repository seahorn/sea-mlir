// --lower-opt=false: the load must reach the VC (canonicalize would forward
// the stored value to it).
// RUN: sea-vc %s --lower --lower-opt=false --seam --seam-stats | FileCheck %s
// RUN: sea-vc %s --lower --lower-opt=false --seam --seam-wide --seam-stats | FileCheck %s

// A node whose base is an ite of nodes (a GEP over a phi of GEPs). The
// rules read it through its flat form, and that form must flatten the
// nodes in the ite's leaves too: else the access's A-INB condition (ARM
// over the ite) sees an unknown term and none is stated; five leaves, more
// than maxPullLeaves, so the ite stays under the node (vcc
// byte_cursor_advance lost one; docs/design/PtrIndex.md section 0a).
// CHECK: seam.inb: assumed accesses=2 conditions=1
// CHECK: result: unsat
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @ite_base {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 16 : i64
  %a = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %n align 8 [2] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [3] : i64
  %j = sea.nondet "j" [4] : i64
  %c1 = sea.nondet "c" [5] : i1
  %c2 = sea.nondet "c" [6] : i1
  %c3 = sea.nondet "c" [7] : i1
  %c4 = sea.nondet "c" [8] : i1
  %pa = sea.ptr.index %a [%i] strides [1] offset 0 inbounds : !sea.ptr<64>, i64
  %pb = sea.ptr.index %b [%i] strides [1] offset 0 inbounds : !sea.ptr<64>, i64
  %s3 = sea.select %c4, %pa, %b : !sea.ptr<64>
  %s2 = sea.select %c3, %pb, %s3 : !sea.ptr<64>
  %s1 = sea.select %c2, %a, %s2 : !sea.ptr<64>
  %p = sea.select %c1, %pa, %s1 : !sea.ptr<64>
  %q = sea.ptr.index %p [%j] strides [1] offset 0 inbounds : !sea.ptr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%q], %v inplace : !pm, i8
  %x = sea.load %m1[%q] : !pm -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
