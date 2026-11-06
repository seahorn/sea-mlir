// RUN: sea-vc %s | FileCheck %s

!m8 = !sea.mem<!sea.addr<8>, i8>

// Memory versions merge at the join; the branch taken decides the cell.
// CHECK: result: sat
// CHECK: nondet: site=1 callee=nondet_bool width=1 value=0
// CHECK: replay: assert-failed
// CHECK: validate: checked={{[0-9]+}} mismatches=0
sea.program @merge {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %c = sea.nondet "nondet_bool" [1] : i1
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %one = sea.const 1 : i8
  %two = sea.const 2 : i8
  sea.cond_br %c, ^t, ^e
^t:
  %m1 = sea.store %m0[%a], %one inplace : !m8, i8
  sea.br ^j(%m1 : !m8)
^e:
  %m2 = sea.store %m0[%a], %two inplace : !m8, i8
  sea.br ^j(%m2 : !m8)
^j(%m3 : !m8):
  %x = sea.load %m3[%a] : !m8 -> i8
  %ok = sea.icmp eq, %x, %one : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
