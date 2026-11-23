// RUN: not sea-vc %s --vc-canonical-sums --vc-defs=nest 2>&1 | FileCheck %s

// --vc-canonical-sums is not combined with --vc-defs: the canonical form
// would flatten the nested and named sums (docs/design/CanonicalSums.md).
// CHECK: --vc-canonical-sums is not combined with --vc-defs
sea.program @p {
  %x = sea.nondet "f" [0] : i8
  %c1 = sea.const 1 : i8
  %s = sea.add %x, %c1 : i8
  %ok = sea.icmp ne, %s, %c1 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
