// RUN: sea-vc %s --vc-canonical-sums --vc-canonical-sums-inner=shared-leaf --smt2 2>%t.s | tr '\n' ' ' | tr -s ' ' | FileCheck %s --check-prefix=SHARED
// RUN: FileCheck %s --check-prefix=SHARED-STAT < %t.s
// RUN: sea-vc %s --vc-canonical-sums --smt2 2>%t.f | tr '\n' ' ' | tr -s ' ' | FileCheck %s --check-prefix=FLAT
// RUN: FileCheck %s --check-prefix=FLAT-STAT < %t.f
// RUN: sea-vc %s --vc-canonical-sums --vc-canonical-sums-inner=shared-leaf | FileCheck %s --check-prefix=SOLVE
// RUN: not sea-vc %s --vc-canonical-sums-inner=shared-leaf 2>&1 | FileCheck %s --check-prefix=ALONE

// --vc-canonical-sums-inner (docs/design/CanonicalSums.md, "Shared inner
// sums"): d = x - y is used by d + z and d + w. shared-leaf keeps d as one
// canonical node, a leaf of both sums; flatten merges it into each.
// SHARED: (bvadd nd.2_0 (bvadd nd.0_0 (bvnot nd.1_0) #x01))
// SHARED: (bvadd nd.3_0 (bvadd nd.0_0 (bvnot nd.1_0) #x01))
// SHARED-STAT: vc.canonical-sums: on {{.*}}inner=shared-leaf
// SHARED-STAT: vc.canonical-sums-inner-shared: 1
// FLAT: (bvadd nd.0_0 nd.2_0 (bvnot nd.1_0) #x01)
// FLAT: (bvadd nd.0_0 nd.3_0 (bvnot nd.1_0) #x01)
// FLAT-STAT: vc.canonical-sums: on {{.*}}inner=flatten
// FLAT-STAT: vc.canonical-sums-inner-shared: 0
// SOLVE: validate: checked={{[0-9]+}} mismatches=0
// SOLVE: result: sat
// ALONE: --vc-canonical-sums-inner needs --vc-canonical-sums
sea.program @p {
  %x = sea.nondet "f" [0] : i8
  %y = sea.nondet "f" [1] : i8
  %z = sea.nondet "f" [2] : i8
  %w = sea.nondet "f" [3] : i8
  %c9 = sea.const 9 : i8
  %d = sea.sub %x, %y : i8
  %a = sea.add %d, %z : i8
  %b = sea.add %d, %w : i8
  %ua = sea.icmp ult, %a, %c9 : i8
  sea.assume %ua
  %ub = sea.icmp ult, %b, %c9 : i8
  sea.assume %ub
  %c0 = sea.const 0 : i8
  %ok = sea.icmp ne, %x, %c0 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
