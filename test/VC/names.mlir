// RUN: sea-vc %s --smt2 --vc-join=eq | FileCheck %s
// RUN: sea-vc %s --smt2 | FileCheck %s --check-prefix=ITE

// VC symbols are the names sea-opt --mlir-use-nameloc-as-prefix prints;
// nondets are named by site.
// CHECK-DAG: (declare-fun nd.7_0 () (_ BitVec 8))
// CHECK-DAG: (declare-fun %y_0 () (_ BitVec 8))
// CHECK-DAG: (declare-fun ^bb1_0 () Bool)
// With ite joins (the default) a block argument is a term, not a constant.
// ITE-NOT: declare-fun %y
sea.program @main {
  %x = sea.nondet "nondet_u8" [7] : i8
  %b = sea.nondet "nondet_bool" [8] : i1
  sea.cond_br %b, ^then, ^else
^then:
  sea.br ^join(%x : i8)
^else:
  %c = sea.const 3 : i8
  sea.br ^join(%c : i8)
^join(%y : i8 loc("y")):
  %c0 = sea.const 0 : i8
  %ok = sea.icmp ne, %y, %c0 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
