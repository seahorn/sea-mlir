// RUN: sea-vc %s --vc-canonical-sums --smt2 2>%t.bal | tr '\n' ' ' | tr -s ' ' | FileCheck %s --check-prefix=BAL
// RUN: FileCheck %s --check-prefix=BAL-STAT < %t.bal
// RUN: sea-vc %s --vc-canonical-sums --vc-canonical-sums-eq=unshared --smt2 2>%t.uns | tr '\n' ' ' | tr -s ' ' | FileCheck %s --check-prefix=UNS
// RUN: FileCheck %s --check-prefix=UNS-STAT < %t.uns
// RUN: sea-vc %s --vc-canonical-sums --vc-canonical-sums-eq=keep --smt2 2>%t.keep | tr '\n' ' ' | tr -s ' ' | FileCheck %s --check-prefix=KEEP
// RUN: FileCheck %s --check-prefix=KEEP-STAT < %t.keep
// RUN: sea-vc %s --vc-canonical-sums --vc-canonical-sums-eq=unshared | FileCheck %s --check-prefix=SOLVE
// RUN: not sea-vc %s --vc-canonical-sums-eq=keep 2>&1 | FileCheck %s --check-prefix=ALONE

// --vc-canonical-sums-eq (docs/design/CanonicalSums.md, "Balancing
// equalities"). x - y = z, where x - y is also compared (shared), and
// a - b = c, where a - b has no other use.
// balance: both balanced.
// BAL-DAG: (= (bvadd nd.1_0 nd.2_0) nd.0_0)
// BAL-DAG: (= (bvadd nd.4_0 nd.5_0) nd.3_0)
// BAL-STAT: vc.canonical-sums: on neg=bvnot eq=balance
// BAL-STAT: vc.canonical-sums-eqs-balanced: 2
// BAL-STAT: vc.canonical-sums-eqs-kept-shared: 0
// BAL-STAT: vc.canonical-sums-eqs-kept: 0
// unshared: x - y is used twice, z once but not a sum: kept, with its
// canonical side; a - b is used once: balanced.
// UNS-DAG: (= (bvadd nd.0_0 (bvnot nd.1_0) #x01) nd.2_0)
// UNS-DAG: (= (bvadd nd.4_0 nd.5_0) nd.3_0)
// UNS-STAT: vc.canonical-sums: on neg=bvnot eq=unshared
// UNS-STAT: vc.canonical-sums-eqs-balanced: 1
// UNS-STAT: vc.canonical-sums-eqs-kept-shared: 1
// UNS-STAT: vc.canonical-sums-eqs-kept: 0
// keep: no equality balanced.
// KEEP-DAG: (= (bvadd nd.0_0 (bvnot nd.1_0) #x01) nd.2_0)
// KEEP-DAG: (= (bvadd nd.3_0 (bvnot nd.4_0) #x01) nd.5_0)
// KEEP-STAT: vc.canonical-sums: on neg=bvnot eq=keep
// KEEP-STAT: vc.canonical-sums-eqs-balanced: 0
// KEEP-STAT: vc.canonical-sums-eqs-kept-shared: 0
// KEEP-STAT: vc.canonical-sums-eqs-kept: 2
// SOLVE: validate: checked={{[0-9]+}} mismatches=0
// SOLVE: result: sat
// ALONE: --vc-canonical-sums-eq needs --vc-canonical-sums
sea.program @p {
  %x = sea.nondet "f" [0] : i8
  %y = sea.nondet "f" [1] : i8
  %z = sea.nondet "f" [2] : i8
  %a = sea.nondet "f" [3] : i8
  %b = sea.nondet "f" [4] : i8
  %c = sea.nondet "f" [5] : i8
  %s = sea.sub %x, %y : i8
  %e1 = sea.icmp eq, %s, %z : i8
  sea.assume %e1
  %c9 = sea.const 9 : i8
  %u = sea.icmp ult, %s, %c9 : i8
  sea.assume %u
  %t = sea.sub %a, %b : i8
  %e2 = sea.icmp eq, %t, %c : i8
  sea.assume %e2
  %c0 = sea.const 0 : i8
  %ok = sea.icmp ne, %z, %c0 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
