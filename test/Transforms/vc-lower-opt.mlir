// RUN: sea-opt %s --pass-pipeline='builtin.module(sea-vc-lower)' | FileCheck %s --check-prefix=OPT
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea-vc-lower{opt=false})' | FileCheck %s --check-prefix=NOOPT
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VC
// RUN: sea-vc %s --lower --lower-opt=false | FileCheck %s --check-prefix=VC

// The lowering's clean-up (cse after sccp; canonicalize + cse before the
// final sea-split-critical-edges; sea-vc --lower-opt): equal loads merge,
// a dead load and an unreachable block go, and the CFG is still in BMC form
// (the cond_br's edge to the join stays split). Two sea.mem.const of the
// same value are two memories (allocations: never merged), each consumed
// in place.
// OPT-LABEL: sea.program @main
// OPT-COUNT-2: sea.mem.const
// OPT-COUNT-1: sea.load
// OPT-NOT: sea.load
// OPT-NOT: sea.const 99
// NOOPT-LABEL: sea.program @main
// NOOPT-COUNT-4: sea.load
// NOOPT: sea.const 99
// VC: result: sat
// VC: replay: assert-failed
// VC: validate: checked={{[0-9]+}} mismatches=0
!m = !sea.mem<!sea.addr<8>, i8>
sea.program @main {
  %z = sea.const 0 : i8
  %one = sea.const 1 : i8
  %m0 = sea.mem.const %z : !m
  %n0 = sea.mem.const %z : !m
  %a = sea.nondet "a" [0] : i8
  %c = sea.nondet "c" [1] : i1
  %p = sea.addr.from_int %a : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%p], %one inplace : !m, i8
  %n1 = sea.store %n0[%p], %one inplace : !m, i8
  %x = sea.load %m1[%p] : !m -> i8
  %y = sea.load %m1[%p] : !m -> i8
  %dead = sea.load %n1[%p] : !m -> i8
  sea.cond_br %c, ^then, ^join(%x : i8)
^then:
  %w = sea.load %m1[%p] : !m -> i8
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
