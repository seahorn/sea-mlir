
!m = !sea.mem<!sea.addr<8>, i8>
sea.program @main {
  %z = sea.const 0 : i8
  %one = sea.const 1 : i8
  %m0 = sea.mem.const %z : !m
  %n0 = sea.mem.const %z : !m
  %a = sea.nondet "a" [0] : i8
  %c = sea.nondet "c" [1] : i1
  %p = sea.addr.from_int %a : i8 to !sea.addr<8>
  %b = sea.nondet "b" [2] : i8
  %q = sea.addr.from_int %b : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%p], %one inplace : !m, i8
  %n1 = sea.store %n0[%p], %one inplace : !m, i8
  %x = sea.load %m1[%q] : !m -> i8
  %y = sea.load %m1[%q] : !m -> i8
  %dead = sea.load %n1[%q] : !m -> i8
  sea.cond_br %c, ^then, ^join(%x : i8)
^then:
  %w = sea.load %m1[%q] : !m -> i8
  %s = sea.add %w, %y : i8
  sea.br ^join(%s : i8)
^gone:
  %k = sea.const 99 : i8
  sea.br ^join(%k : i8)
^join(%r : i8):
  %ok = sea.icmp ne, %r, %one : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
