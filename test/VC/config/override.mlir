// The configs' options come first: the command line overrides a config's
// value (--seam=false after a config with --seam), and of two configs the
// later one wins; each loaded config is printed, in order.
// RUN: sea-vc %s --config=%S/Inputs/profiles/seam-trace.cfg --seam=false | FileCheck %s --check-prefix=CMD
// RUN: sea-vc %s --config=%S/Inputs/profiles/seam-trace.cfg --config=%S/Inputs/profiles/stats.cfg | FileCheck %s --check-prefix=TWO
// CMD-NOT: seam.inb
// CMD: result: sat
// TWO: vc.config: {{.*}}seam-trace.cfg
// TWO-NEXT: vc.config: {{.*}}stats.cfg
// TWO: vc.dag-size:
// TWO: seam.inb: assumed
// TWO: result: sat
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
