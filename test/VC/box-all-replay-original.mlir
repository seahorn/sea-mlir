// RUN: sea-vc %s --lower --replay-original 2>&1 | FileCheck %s
// --replay-original on a box the interpreter cannot enumerate (about 2^62
// indices): the lowered program's sat replays (one index); the original's
// run ends at the step limit, which is undetermined, not a mismatch
// (docs/design/BoxAll.md).
// CHECK: result: sat
// CHECK: replay: assert-failed
// CHECK: replay.original: status=step-limit {{.*}}match=undetermined
// CHECK: [vc.replay-original-undetermined]
sea.program @p {
  %m = sea.nondet "m" [0] : i64
  %x = sea.nondet "x" [1] : i64
  %c5 = sea.const 5 : i64
  %big = sea.const 4611686018427387904 : i64
  %a = sea.icmp eq, %x, %c5 : i64
  %b = sea.icmp uge, %m, %big : i64
  %ab = sea.and %a, %b : i1
  sea.assume %ab
  %all = sea.box.all (%i) = (2) to (%m) step (3) {
    %e = sea.icmp ne, %i, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
