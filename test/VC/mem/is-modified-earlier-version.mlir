// RUN: bash -c 'sea-vc %s --lower > %t.out 2>&1; test $? -eq 2'
// RUN: FileCheck %s < %t.out

// sea.mem.is_modified of an earlier memory version in sea-vc's replay: the
// lowering (sea-lower-tracking) turns it into a load of the shadow memory,
// so the replay counts it as a load of an earlier version and names it at
// the is_modified's location. The VC reads the version named (bit 0), the
// in-place interpreter the current shadow (1, after the tracked store): the
// no-clobber claim fails.
// CHECK-NOT: interp.earlier-version-is-modified
// CHECK: interp.earlier-version-loads: 1
// CHECK-NOT: interp.earlier-version-is-modified
// CHECK: replay: no-clobber-violated: {{.*}}is-modified-earlier-version.mlir:[[#@LINE+11]]:8 (a load of an earlier memory version
// CHECK: the cause is the program's Mem-SSA no-clobber claim, not the VC{{.*}}[vc.replay-mismatch]
// CHECK-NOT: interp.earlier-version-is-modified
// CHECK: result: unknown
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @earlier {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 8 : i64
  %a = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%a], %v inplace tracked : !m, i8
  %t = sea.mem.is_modified %m0[%a] : !m
  %f = sea.const 1 : i1
  %ok = sea.icmp eq, %t, %f : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
