// RUN: sea-run %s --program=earlier --missing=zero --log=warn.interp.is-modified-earlier-version:2 2>&1 | FileCheck %s
// RUN: sea-run %s --program=current --missing=zero 2>&1 | FileCheck %s --check-prefix=CUR

// sea.mem.is_modified of an earlier memory version (Memory.md, "Loads of a
// version after its consumer"). The version %m0 has no tracked write to
// %a, so its bit is 0; the in-place interpreter reads the current
// modification set, after the tracked store of %m1 (1): documented
// behavior, warned once per executed op
// (interp.is-modified-earlier-version), so the run fails the assertion.
// CHECK-COUNT-1: Warning: is_modified of an earlier memory version{{.*}}[interp.is-modified-earlier-version]
// CHECK-NOT: interp.is-modified-earlier-version
// CHECK: status: assert-failed
// CHECK-NOT: interp.is-modified-earlier-version
// is_modified of the current version: no warning.
// CUR-NOT: interp.is-modified-earlier-version
// CUR: status: halted
// CUR-NOT: interp.is-modified-earlier-version
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @earlier {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 8 : i64
  %a = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%a], %v inplace tracked : !m, i8
  %t = sea.mem.is_modified %m0[%a] : !m
  %f = sea.const 0 : i1
  %ok = sea.icmp eq, %t, %f : i1
  sea.assert %ok
  sea.halt
}
sea.program @current {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 8 : i64
  %a = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %v = sea.const 1 : i8
  %m1 = sea.store %m0[%a], %v inplace tracked : !m, i8
  %t = sea.mem.is_modified %m1[%a] : !m
  %one = sea.const 1 : i1
  %ok = sea.icmp eq, %t, %one : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
