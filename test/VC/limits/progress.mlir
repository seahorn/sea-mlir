// --progress: `phase: <name>` on stderr as each phase starts, in order; none
// without the flag.
// RUN: sea-vc %s --lower --progress 2>&1 >/dev/null | FileCheck %s
// RUN: sea-vc %s --lower 2>&1 | FileCheck %s --check-prefix=QUIET
// CHECK: phase: lower
// CHECK: phase: encode
// CHECK: phase: solver-term
// CHECK: phase: ackermann
// CHECK: phase: solve
// CHECK: phase: replay
// QUIET-NOT: phase:
sea.program @p {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c100 = sea.const 100 : i8
  %y = sea.add %x, %c100 : i8
  %ok = sea.icmp uge, %y, %c100 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
