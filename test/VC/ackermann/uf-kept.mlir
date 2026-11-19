// --ro-arrays=uf (docs/design/Ackermann.md, "Read-only arrays as functions")
// keeps an array that is written: %m0 is stored to (the query has a store
// over it), %h0 is only read. The first stays an array, the second becomes a
// function; the sat model rebuilds %h0 and the solver's model gives %m0.
// RUN: sea-vc %s --ackermannize=off --ro-arrays=uf --smt2 2>/dev/null | FileCheck %s --check-prefix=SMT
// RUN: sea-vc %s --ackermannize=off --ro-arrays=uf 2>&1 | FileCheck %s
// RUN: %if yices %{ sea-vc %s --ackermannize=off --ro-arrays=uf --solver=yices 2>&1 | FileCheck %s %}
// RUN: %if bitwuzla %{ sea-vc %s --ackermannize=off --ro-arrays=uf --solver=bitwuzla 2>&1 | FileCheck %s %}
// SMT-DAG: (declare-fun nd.0{{[_0-9]*}} () (Array (_ BitVec 3) (_ BitVec 8)))
// SMT-DAG: (declare-fun uf.nd.1{{[_0-9]*}} ((_ BitVec 3)) (_ BitVec 8))
// CHECK: vc.ro-arrays: uf=1 kept-array=1
// CHECK: result: sat
// CHECK: vc.ackermann-model: defaulted={{[0-9]+}} inconsistent=0
// CHECK: replay: assert-failed
// CHECK: validate: checked={{[0-9]+}} mismatches=0
!am = !sea.mem<!sea.addr<3>, i8>
sea.program @ufkept {
  %m0 = sea.mem.nondet "m" [0] : !am
  %h0 = sea.mem.nondet "h" [1] : !am
  %pi = sea.nondet "p" [2] : i3
  %ri = sea.nondet "r" [3] : i3
  %p = sea.addr.from_int %pi : i3 to !sea.addr<3>
  %r = sea.addr.from_int %ri : i3 to !sea.addr<3>
  %c5 = sea.const 5 : i8
  %m1 = sea.store %m0[%p], %c5 inplace : !am, i8
  %x = sea.load %m1[%r] : !am -> i8
  %y = sea.load %h0[%r] : !am -> i8
  // fails e.g. with r = p and h[r] = 5
  %bad = sea.icmp eq, %x, %y : i8
  %t = sea.const 1 : i1
  %ok = sea.xor %bad, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
