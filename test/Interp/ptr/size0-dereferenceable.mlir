// RUN: sea-run %s --program=main | FileCheck %s
// RUN: sea-opt %s --sea-lower-fat-pointers --sea-place-static | sea-run - --program=main | FileCheck %s
// RUN: sea-vc %s --program=any_len --lower --lower-opt=false | FileCheck %s --check-prefix=VC

// A pointer of sea.ptr.nondet or sea.func_addr points to an object of size
// 0: sea.ptr.dereferenceable is 0 for every len > 0, and 1 for len 0 (the
// general rule: offset <= size and len <= size - offset).
// CHECK-DAG: print: nondet.len0 = true
// CHECK-DAG: print: nondet.len1 = false
// CHECK-DAG: print: func.len0 = true
// CHECK-DAG: print: func.len1 = false
// CHECK: status: halted
// @any_len: in the VC, for a nondet len, dereferenceable is 1 iff len = 0.
// VC: result: unsat
sea.program @main {
  %p = sea.ptr.nondet "nd_voidp" [0] : !sea.ptr<64>
  %f = sea.func_addr @foo : !sea.ptr<64>
  %zero = sea.const 0 : i64
  %one = sea.const 1 : i64
  %p0 = sea.ptr.dereferenceable %p, %zero : !sea.ptr<64>, i64
  %p1 = sea.ptr.dereferenceable %p, %one : !sea.ptr<64>, i64
  %f0 = sea.ptr.dereferenceable %f, %zero : !sea.ptr<64>, i64
  %f1 = sea.ptr.dereferenceable %f, %one : !sea.ptr<64>, i64
  sea.print "nondet.len0" %p0 : i1
  sea.print "nondet.len1" %p1 : i1
  sea.print "func.len0" %f0 : i1
  sea.print "func.len1" %f1 : i1
  %t = sea.const 1 : i1
  %n1 = sea.xor %p1, %t : i1
  %m1 = sea.xor %f1, %t : i1
  %a = sea.and %p0, %n1 : i1
  %b = sea.and %f0, %m1 : i1
  %ok = sea.and %a, %b : i1
  sea.assert %ok
  sea.halt
}

sea.program @any_len {
  %p = sea.ptr.nondet "nd_voidp" [0] : !sea.ptr<64>
  %f = sea.func_addr @foo : !sea.ptr<64>
  %n = sea.nondet "len" [1] : i64
  %zero = sea.const 0 : i64
  %z = sea.icmp eq, %n, %zero : i64
  %dp = sea.ptr.dereferenceable %p, %n : !sea.ptr<64>, i64
  %df = sea.ptr.dereferenceable %f, %n : !sea.ptr<64>, i64
  %ep = sea.icmp eq, %dp, %z : i1
  %ef = sea.icmp eq, %df, %z : i1
  %ok = sea.and %ep, %ef : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
