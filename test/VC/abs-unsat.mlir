// RUN: sea-vc %s --lower | FileCheck %s

// Facts of sea.abs that hold for every input: the result is x or 0 - x,
// it is at most 2^(w-1) unsigned, and it is negative only at the minimal
// signed value (where it is that value).
// CHECK: result: unsat
sea.program @main {
  %x = sea.nondet "nondet_i8" [0] : i8
  %z = sea.const 0 : i8
  %m = sea.const -128 : i8
  %a = sea.abs %x : i8
  %n = sea.sub %z, %x : i8
  %e0 = sea.icmp eq, %a, %x : i8
  %e1 = sea.icmp eq, %a, %n : i8
  %c0 = sea.or %e0, %e1 : i1
  %c1 = sea.icmp ule, %a, %m : i8
  %neg = sea.icmp slt, %a, %z : i8
  %xm = sea.icmp eq, %x, %m : i8
  %c2 = sea.icmp eq, %neg, %xm : i1
  %a01 = sea.and %c0, %c1 : i1
  %all = sea.and %a01, %c2 : i1
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
