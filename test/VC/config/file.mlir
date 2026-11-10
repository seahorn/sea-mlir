// --config=<file>: its options apply (--progress), and vc.config names the
// resolved path.
// RUN: sea-vc %s --config=%S/Inputs/progress.cfg 2>/dev/null | FileCheck %s
// RUN: sea-vc %s --config=%S/Inputs/progress.cfg 2>&1 >/dev/null | FileCheck %s --check-prefix=ERR
// CHECK: vc.config: {{.*}}Inputs{{/|\\}}progress.cfg
// CHECK: result: sat
// ERR: log: vc.phase start phase=solve{{$}}
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
