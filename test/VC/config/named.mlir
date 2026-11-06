// A bare name is <name>.cfg in $SEA_MLIR_PROFILE_PATH (colon-separated, in
// order; a missing directory is skipped).
// RUN: env SEA_MLIR_PROFILE_PATH=%S/Inputs/none:%S/Inputs/profiles sea-vc %s --config=stats | FileCheck %s
// CHECK: vc.config: {{.*}}profiles{{/|\\}}stats.cfg
// CHECK: vc.dag-size:
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
