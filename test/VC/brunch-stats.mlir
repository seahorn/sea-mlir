// RUN: sea-vc %s --brunch-stats | FileCheck %s
// RUN: sea-vc %s | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --lower --seam --seam-stats --brunch-stats | FileCheck %s --check-prefix=SEAM

// --brunch-stats adds BRUNCH_STAT <key> <value> lines; without it none.
// CHECK: BRUNCH_STAT vc-time {{[0-9.]+}}
// CHECK: BRUNCH_STAT solve-time {{[0-9.]+}}
// CHECK: BRUNCH_STAT result unsat
// CHECK: BRUNCH_STAT status 0
// PLAIN-NOT: BRUNCH_STAT
// SEAM: BRUNCH_STAT seam.inb assumed
// SEAM: BRUNCH_STAT seam.inb-accesses {{[0-9]+}}
// SEAM: BRUNCH_STAT seam.rewrite-time {{[0-9.]+}}
// SEAM: BRUNCH_STAT seam.rules-fired {{[0-9]+}}
// SEAM: BRUNCH_STAT result unsat
sea.program @p {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.add %x, %x : i8
  %z = sea.sub %y, %x : i8
  %ok = sea.icmp eq, %z, %x : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
