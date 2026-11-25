// RUN: sea-vc %s --vc-canonical-sums --smt2 2>%t.err | tr '\n' ' ' | tr -s ' ' | FileCheck %s --implicit-check-not=bvadd
// RUN: FileCheck %s --check-prefix=STAT < %t.err

// Item 5 of the form (docs/design/CanonicalSums.md): bitwise terms are left
// alone. ~x = y (an xor with all ones in the VC) is not a sum: it is not
// rewritten into an adder, and no equality is balanced.
// CHECK: (= (bvxor nd.0_0 #xff) nd.1_0)
// STAT: vc.canonical-sums: on
// STAT: vc.canonical-sums-sums: 0
// STAT: vc.canonical-sums-eqs-balanced: 0
sea.program @p {
  %x = sea.nondet "f" [0] : i8
  %y = sea.nondet "f" [1] : i8
  %m = sea.const -1 : i8
  %nx = sea.xor %x, %m : i8
  %e = sea.icmp eq, %nx, %y : i8
  sea.assume %e
  %c0 = sea.const 0 : i8
  %ok = sea.icmp ne, %y, %c0 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
