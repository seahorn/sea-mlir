// @file in a config is relative to the including config's directory, and
// nested: sub/outer.cfg includes ../inner.cfg (--lower) and adds --seam.
// RUN: sea-vc %s --config=%S/Inputs/profiles/sub/outer.cfg | FileCheck %s
// CHECK: vc.config: {{.*}}outer.cfg
// CHECK-NOT: vc.config:
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
