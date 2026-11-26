// REQUIRES: yices
// RUN: sea-vc %s --program=store --solver=yices | FileCheck %s
// RUN: sea-vc %s --program=join --lower --vc-join=eq --solver=yices | FileCheck %s
// RUN: sea-vc %s --program=store --solver=z3 | FileCheck %s
// RUN: sea-vc %s --program=join --lower --vc-join=eq --solver=z3 | FileCheck %s
// RUN: %if bitwuzla %{ sea-vc %s --program=store --solver=bitwuzla | FileCheck %s %}
// RUN: %if bitwuzla %{ sea-vc %s --program=join --lower --vc-join=eq --solver=bitwuzla | FileCheck %s %}

// A sat query over a constant memory (sea.mem.const = a const-array) is
// replayed with Yices: the replay evaluates the VC's load term, a select
// over a store chain whose leaf is the const-array, in the model. Yices has
// no const-array (its only encoding is a lambda, which Yices refuses to
// evaluate: "term contains lambdas"), so the backend expands the select in
// model evaluation as it does in assertions. z3 and Bitwuzla, which have
// const-arrays, give the same replay.
// CHECK: solver-answer: sat
// CHECK: replay: assert-failed
// CHECK: validate: checked={{[0-9]+}} mismatches=0
// CHECK: result: sat

// A reduced form of the release smoke program's mutant (no i == j).
sea.program @store {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !sea.mem<!sea.addr<8>, i8>
  %a = sea.nondet "a" [0] : i8
  %b = sea.nondet "b" [1] : i8
  %v = sea.nondet "v" [2] : i8
  %p = sea.addr.from_int %a : i8 to !sea.addr<8>
  %q = sea.addr.from_int %b : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%p], %v inplace : !sea.mem<!sea.addr<8>, i8>, i8
  %x = sea.load %m1[%q] : !sea.mem<!sea.addr<8>, i8> -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// A constant memory reaching a join, with a wrong expectation on the write
// path. --vc-join=eq: the joined memory is defined by guarded equalities,
// which Yices inlines (ConstArrayDefs) before the select expansion.
sea.program @join {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !sea.mem<!sea.addr<8>, i8>
  %c = sea.nondet "c" [0] : i1
  %a = sea.nondet "a" [1] : i8
  %p = sea.addr.from_int %a : i8 to !sea.addr<8>
  %v = sea.const 5 : i8
  sea.cond_br %c, ^w, ^j(%m0 : !sea.mem<!sea.addr<8>, i8>)
^w:
  %m1 = sea.store %m0[%p], %v inplace : !sea.mem<!sea.addr<8>, i8>, i8
  sea.br ^j(%m1 : !sea.mem<!sea.addr<8>, i8>)
^j(%m2: !sea.mem<!sea.addr<8>, i8>):
  %x = sea.load %m2[%p] : !sea.mem<!sea.addr<8>, i8> -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
