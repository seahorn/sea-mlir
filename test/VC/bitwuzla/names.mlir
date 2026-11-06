// REQUIRES: bitwuzla
// RUN: sea-vc %s --smt2 --solver=bitwuzla --vc-join=eq | FileCheck %s

// test/VC/names.mlir with Bitwuzla (it prints declare-const). VC symbols are the names sea-opt --mlir-use-nameloc-as-prefix prints;
// nondets are named by site.
// CHECK-DAG: (declare-const nd.7_0 (_ BitVec 8))
// CHECK-DAG: (declare-const %y_0 (_ BitVec 8))
// CHECK-DAG: (declare-const ^bb1_0 Bool)
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
