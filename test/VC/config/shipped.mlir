// The shipped example profiles (profiles/, found from the build tree):
// seam-wide includes seam (--lower --seam) and adds --seam-wide.
// RUN: env -u SEA_MLIR_PROFILE_PATH sea-vc %s --config=seam-wide | FileCheck %s
// CHECK: vc.config: {{.*}}seam-wide.cfg
// CHECK: seam.inb: assumed
// CHECK: result: sat
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
