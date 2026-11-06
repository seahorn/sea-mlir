// Model reconstruction (docs/design/Ackermann.md) for a term the query does
// not hold: %y reads a store over the initial memory but nothing uses it, so
// the query reads %m0 only under select (lowered), while validation evaluates
// select(store(m0, q, 5), r). The read is evaluated concretely through the
// store (no constant-array term, which Yices could not evaluate); r != q
// makes the read pass the store and read m0 at an index the query does not
// read (default 0).
// RUN: sea-vc %s --ackermannize=on 2>&1 | FileCheck %s
// RUN: %if yices %{ sea-vc %s --ackermannize=on --solver=yices 2>&1 | FileCheck %s %}
// RUN: sea-vc %s 2>&1 | FileCheck %s --check-prefix=AUTO
// CHECK: vc.ackermann: on
// CHECK: vc.ackermann-arrays: 1
// CHECK: result: sat
// CHECK: vc.ackermann-model: defaulted={{[0-9]+}} inconsistent=0
// CHECK: replay: assert-failed
// CHECK: validate: checked={{[0-9]+}} mismatches=0
// AUTO: vc.ackermann: on
// AUTO: mismatches=0
!am = !sea.mem<!sea.addr<3>, i8>
sea.program @replaystore {
  %m0 = sea.mem.nondet "m" [0] : !am
  %pi = sea.nondet "p" [1] : i3
  %qi = sea.nondet "q" [2] : i3
  %ri = sea.nondet "r" [3] : i3
  %p = sea.addr.from_int %pi : i3 to !sea.addr<3>
  %q = sea.addr.from_int %qi : i3 to !sea.addr<3>
  %r = sea.addr.from_int %ri : i3 to !sea.addr<3>
  %rq = sea.icmp ne, %ri, %qi : i3
  sea.assume %rq
  %x = sea.load %m0[%p] : !am -> i8
  %c5 = sea.const 5 : i8
  %m1 = sea.store %m0[%q], %c5 inplace : !am, i8
  %y = sea.load %m1[%r] : !am -> i8
  %c3 = sea.const 3 : i8
  %ok = sea.icmp ne, %x, %c3 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
