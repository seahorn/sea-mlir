// RUN: sea-run %s --program=earlier --log=warn.interp.memmove-earlier-version:2 2>&1 | FileCheck %s
// RUN: sea-vc %s --program=earlier --lower 2>&1 | FileCheck %s --check-prefix=VC
// RUN: sea-run %s --program=current 2>&1 | FileCheck %s --check-prefix=CUR
// RUN: sea-run %s --program=zerolen 2>&1 | FileCheck %s --check-prefix=CUR

// A memmove whose source is an earlier memory version (optimized Mem-SSA,
// Memory.md "Loads of a version after its consumer"). The VC reads the
// version named, %m1 (x = 1: the assertion holds, unsat). The in-place
// interpreter copies the current contents, %m2 (x = 2): documented
// behavior, warned once per executed op (interp.memmove-earlier-version),
// so the run fails the assertion.
// CHECK-COUNT-1: Warning: memmove from an earlier memory version{{.*}}[interp.memmove-earlier-version]
// CHECK-NOT: interp.memmove-earlier-version
// CHECK: status: assert-failed
// CHECK-NOT: interp.memmove-earlier-version
// VC: result: unsat
// A memmove from the current version, or of length 0 from an earlier one
// (it reads nothing): no warning.
// CUR-NOT: interp.memmove-earlier-version
// CUR: status: halted
// CUR-NOT: interp.memmove-earlier-version
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @earlier {
  %m0 = sea.mem.nondet "mem" [1] : !m
  %sz = sea.const 16 : i64
  %p = sea.malloc %sz align 8 [2] : i64 -> !sea.ptr<64>
  %q = sea.alloca %sz align 8 [3] : i64 -> !sea.ptr<64>
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c8 = sea.const 8 : i64
  %m1 = sea.store %m0[%p], %c1 inplace : !m, i64
  %m2 = sea.store %m1[%p], %c2 inplace : !m, i64
  %m3 = sea.memmove %m2[%q], %m1[%p], %c8 inplace : !m
  %x = sea.load %m3[%q] : !m -> i64
  %e = sea.icmp eq, %x, %c1 : i64
  sea.assert %e
  sea.halt
}
sea.program @current {
  %m0 = sea.mem.nondet "mem" [1] : !m
  %sz = sea.const 16 : i64
  %p = sea.malloc %sz align 8 [2] : i64 -> !sea.ptr<64>
  %q = sea.alloca %sz align 8 [3] : i64 -> !sea.ptr<64>
  %c2 = sea.const 2 : i64
  %c8 = sea.const 8 : i64
  %m1 = sea.store %m0[%p], %c2 inplace : !m, i64
  %m2 = sea.memmove %m1[%q], %m1[%p], %c8 inplace : !m
  %x = sea.load %m2[%q] : !m -> i64
  %e = sea.icmp eq, %x, %c2 : i64
  sea.assert %e
  sea.halt
}
sea.program @zerolen {
  %m0 = sea.mem.nondet "mem" [1] : !m
  %sz = sea.const 16 : i64
  %p = sea.malloc %sz align 8 [2] : i64 -> !sea.ptr<64>
  %q = sea.alloca %sz align 8 [3] : i64 -> !sea.ptr<64>
  %c0 = sea.const 0 : i64
  %c2 = sea.const 2 : i64
  %m1 = sea.store %m0[%p], %c2 inplace : !m, i64
  %m2 = sea.memmove %m1[%q], %m0[%p], %c0 inplace : !m
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
