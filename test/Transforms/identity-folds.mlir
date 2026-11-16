// RUN: sea-opt %s --canonicalize | FileCheck %s
// Identity folders and flagged constants (SeaOps.cpp; every case is also
// in utils/check-semantics.py --fold, proved with z3).

// @ident: every result is x or a constant; each is observed by a strong
// print (never erased). The last two: addr.cmp of one address with itself,
// and of the constant addresses 42 and 43.
// CHECK-LABEL: sea.program @ident
// CHECK-DAG: %[[F:.*]] = sea.const false
// CHECK-DAG: %[[T:.*]] = sea.const true
// CHECK-DAG: %[[M:.*]] = sea.const -1 : i8
// CHECK-DAG: %[[Z:.*]] = sea.const 0 : i8
// CHECK-DAG: %[[X:.*]] = sea.nondet "x"
// CHECK-DAG: %[[P:.*]] = sea.nondet "p"
// CHECK-NOT: sea.{{and|or|xor|icmp|addr.cmp|add|sub|mul}}
// CHECK: sea.print "r" %[[Z]] : i8
// CHECK-NEXT: sea.print "r" %[[X]] : i8
// CHECK-NEXT: sea.print "r" %[[M]] : i8
// CHECK-NEXT: sea.print "r" %[[X]] : i8
// CHECK-NEXT: sea.print "r" %[[Z]] : i8
// CHECK-NEXT: sea.print "r" %[[X]] : i8
// CHECK-NEXT: sea.print "r" %[[X]] : i8
// CHECK-NEXT: sea.print "r" %[[Z]] : i8
// CHECK-NEXT: sea.print "r" %[[X]] : i8
// CHECK-NEXT: sea.print "r" %[[Z]] : i8
// CHECK-NEXT: sea.print "b" %[[P]] : i1
// CHECK-NEXT: sea.print "b" %[[T]] : i1
// CHECK-NEXT: sea.print "b" %[[T]] : i1
// CHECK-NEXT: sea.print "b" %[[F]] : i1
// CHECK-NEXT: sea.print "b" %[[F]] : i1
// CHECK-NEXT: sea.print "b" %[[T]] : i1
// CHECK-NEXT: sea.print "b" %[[T]] : i1
// CHECK-NEXT: sea.print "b" %[[F]] : i1
sea.program @ident {
  %c0 = sea.const 0 : i8
  %c1 = sea.const 1 : i8
  %cm = sea.const -1 : i8
  %k = sea.const 42 : i8
  %t = sea.const 1 : i1
  %x = sea.nondet "x" [0] : i8
  %p = sea.nondet "p" [1] : i1
  %a0 = sea.and %x, %c0 : i8
  %a1 = sea.and %cm, %x : i8
  %a2 = sea.or %x, %cm : i8
  %a3 = sea.or %c0, %x : i8
  %a4 = sea.xor %x, %x : i8
  %xk = sea.xor %x, %k : i8
  %a5 = sea.xor %xk, %k : i8
  %a6 = sea.add %x, %c0 nsw : i8
  %a7 = sea.sub %x, %x nuw : i8
  %a8 = sea.mul %c1, %x nsw nuw : i8
  %a9 = sea.mul %x, %c0 : i8
  %b0 = sea.and %t, %p : i1
  %b1 = sea.or %p, %t : i1
  %b2 = sea.icmp eq, %x, %x : i8
  %b3 = sea.icmp slt, %x, %x : i8
  %b4 = sea.icmp ult, %x, %c0 : i8
  %b5 = sea.icmp ule, %c0, %x : i8
  %q = sea.addr.from_int %x : i8 to !sea.addr<8>
  %b6 = sea.addr.cmp uge, %q, %q : !sea.addr<8>
  %q1 = sea.addr.from_int %k : i8 to !sea.addr<8>
  %q2 = sea.addr.add %q1, %c1 : !sea.addr<8>, i8
  %b7 = sea.addr.cmp eq, %q1, %q2 : !sea.addr<8>
  sea.print "r" %a0 : i8
  sea.print "r" %a1 : i8
  sea.print "r" %a2 : i8
  sea.print "r" %a3 : i8
  sea.print "r" %a4 : i8
  sea.print "r" %a5 : i8
  sea.print "r" %a6 : i8
  sea.print "r" %a7 : i8
  sea.print "r" %a8 : i8
  sea.print "r" %a9 : i8
  sea.print "b" %b0 : i1
  sea.print "b" %b1 : i1
  sea.print "b" %b2 : i1
  sea.print "b" %b3 : i1
  sea.print "b" %b4 : i1
  sea.print "b" %b5 : i1
  sea.print "b" %b6 : i1
  sea.print "b" %b7 : i1
  sea.halt
}

// @flags: a flagged op with constant operands folds when its promise
// holds of them (127 + 1 has no unsigned wrap), not when it breaks it
// (127 + 1 nsw wraps: the promise stays for the interpreter and
// sea-wrap-flags); at i1, `x * 1 nsw` wraps for x = 1 (1 is -1 signed):
// kept, while `x * 1` is x.
// CHECK-LABEL: sea.program @flags
// CHECK-DAG: %[[B:.*]] = sea.const -128 : i8
// CHECK-DAG: %[[X:.*]] = sea.nondet "x"
// CHECK-DAG: %[[A:.*]] = sea.add %{{.*}}, %{{.*}} nsw : i8
// CHECK-DAG: %[[C:.*]] = sea.mul %[[X]], %{{.*}} nsw : i1
// CHECK-NOT: nuw
// CHECK: sea.print "r" %[[A]] : i8
// CHECK-NEXT: sea.print "r" %[[B]] : i8
// CHECK-NEXT: sea.print "r" %[[C]] : i1
// CHECK-NEXT: sea.print "r" %[[X]] : i1
// CHECK: sea.halt
sea.program @flags {
  %m = sea.const 127 : i8
  %one = sea.const 1 : i8
  %t = sea.const 1 : i1
  %x = sea.nondet "x" [0] : i1
  %a = sea.add %m, %one nsw : i8
  %b = sea.add %m, %one nuw : i8
  %c = sea.mul %x, %t nsw : i1
  %d = sea.mul %x, %t : i1
  sea.print "r" %a : i8
  sea.print "r" %b : i8
  sea.print "r" %c : i1
  sea.print "r" %d : i1
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
