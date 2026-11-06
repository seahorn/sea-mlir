// The A-INB premise check (--seam-check-inb-premise) is off by default.
// Every cmp.diff-base decision's two
// addresses must lie in accesses whose A-INB condition the run recorded.
// Here a store to object a and a load from object b are decided apart by
// cmp.diff-base; both are program accesses, so both are recorded.
// RUN: sea-vc %s --lower --seam --seam-stats --seam-check-inb-premise 2>&1 | FileCheck %s --check-prefix=ON
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefix=OFF
// ON: seam.rule.premise.inb-checked: {{[1-9]}}
// ON-NOT: premise.inb-unrecorded
// ON-NOT: vc.inb-premise-unrecorded
// ON: result: unsat
// OFF-NOT: premise.inb-checked
// OFF: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @apart {
  %c8 = sea.const 8 : i64
  %a = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [3] : !m
  %i = sea.nondet "i" [4] : i64
  %j = sea.nondet "j" [5] : i64
  %pa = sea.ptr.add %a, %i : !sea.ptr<64>, i64
  %pb = sea.ptr.add %b, %j : !sea.ptr<64>, i64
  %old = sea.load %m0[%pb] : !m -> i8
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%pa], %v inplace : !m, i8
  %x = sea.load %m1[%pb] : !m -> i8
  %ok = sea.icmp eq, %x, %old : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
