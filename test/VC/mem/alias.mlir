// RUN: sea-vc %s --program=alias | FileCheck %s --check-prefix=BUG
// RUN: %if yices %{ sea-vc %s --program=alias --solver=yices | FileCheck %s --check-prefix=BUG %}
// RUN: sea-vc %s --program=distinct | FileCheck %s --check-prefix=SAFE
// RUN: sea-vc %s --program=initial | FileCheck %s --check-prefix=INIT

!m8 = !sea.mem<!sea.addr<8>, i8>

// *p = 1; *q = 2; assert *p == 1 fails exactly when p == q.
// BUG: result: sat
// BUG: nondet: site=1 callee=nd_p width=8 value=[[P:[0-9]+]]
// BUG: nondet: site=2 callee=nd_q width=8 value=[[P]]
// BUG: replay: assert-failed
// BUG: validate: checked={{[0-9]+}} mismatches=0
sea.program @alias {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %i = sea.nondet "nd_p" [1] : i8
  %j = sea.nondet "nd_q" [2] : i8
  %p = sea.addr.from_int %i : i8 to !sea.addr<8>
  %q = sea.addr.from_int %j : i8 to !sea.addr<8>
  %one = sea.const 1 : i8
  %two = sea.const 2 : i8
  %m1 = sea.store %m0[%p], %one inplace : !m8, i8
  %m2 = sea.store %m1[%q], %two inplace : !m8, i8
  %x = sea.load %m2[%p] : !m8 -> i8
  %ok = sea.icmp eq, %x, %one : i8
  sea.assert %ok
  sea.halt
}

// The same with q = p + 1: no aliasing (mod 2^8, p + 1 != p).
// SAFE: result: unsat
sea.program @distinct {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %i = sea.nondet "nd_p" [1] : i8
  %p = sea.addr.from_int %i : i8 to !sea.addr<8>
  %c1 = sea.const 1 : i8
  %q = sea.addr.add %p, %c1 : !sea.addr<8>, i8
  %one = sea.const 1 : i8
  %two = sea.const 2 : i8
  %m1 = sea.store %m0[%p], %one inplace : !m8, i8
  %m2 = sea.store %m1[%q], %two inplace : !m8, i8
  %x = sea.load %m2[%p] : !m8 -> i8
  %ok = sea.icmp eq, %x, %one : i8
  sea.assert %ok
  sea.halt
}

// Initial contents come from the model; replay reads them lazily.
// INIT: result: sat
// INIT: memread: site=0 addr=7 width=8 value=0
// INIT: replay: assert-failed
// INIT: validate: checked={{[0-9]+}} mismatches=0
sea.program @initial {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %c7 = sea.const 7 : i8
  %p = sea.addr.from_int %c7 : i8 to !sea.addr<8>
  %x = sea.load %m0[%p] : !m8 -> i8
  %z = sea.const 0 : i8
  %ok = sea.icmp ne, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
