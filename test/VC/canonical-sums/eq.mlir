// RUN: sea-vc %s --vc-canonical-sums --smt2 2>/dev/null | tr '\n' ' ' | tr -s ' ' | FileCheck %s --implicit-check-not=bvsub
// RUN: sea-vc %s --vc-canonical-sums | FileCheck %s --check-prefix=SOLVE

// Item 3 of the form (docs/design/CanonicalSums.md): an equality with a sum
// side is balanced, never one isolated term. z = 5 - x - y becomes
// x + y + z = 5; x + 1 = y - z becomes x + z + 1 = y (the orientation with
// fewer negative leaves).
// CHECK-DAG: (= (bvadd nd.0_0 nd.1_0 nd.2_0) #x05)
// CHECK-DAG: (= (bvadd nd.0_0 nd.2_0 #x01) nd.1_0)
// SOLVE: vc.canonical-sums-eqs: 2
// SOLVE: vc.canonical-sums-eqs-changed: 2
// SOLVE: validate: checked={{[0-9]+}} mismatches=0
// SOLVE: result: sat
sea.program @p {
  %x = sea.nondet "f" [0] : i8
  %y = sea.nondet "f" [1] : i8
  %z = sea.nondet "f" [2] : i8
  %c1 = sea.const 1 : i8
  %c5 = sea.const 5 : i8
  %a = sea.sub %c5, %x : i8
  %b = sea.sub %a, %y : i8
  %e1 = sea.icmp eq, %z, %b : i8
  sea.assume %e1
  %l = sea.add %x, %c1 : i8
  %r = sea.sub %y, %z : i8
  %e2 = sea.icmp eq, %l, %r : i8
  sea.assume %e2
  %c0 = sea.const 0 : i8
  %ok = sea.icmp ne, %x, %c0 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
