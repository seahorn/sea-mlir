// sea-vc replays a sat model with validation, which checks the value of
// every load of an earlier memory version (Mem-SSA no-clobber claim): such
// loads are counted (interp.earlier-version-loads), not warned per load.
// sea-run, which does not validate, still warns.
// RUN: sea-vc %s 2>&1 | FileCheck %s
// RUN: sea-run %s --missing=zero 2>&1 | FileCheck %s --check-prefix=INTERP
// CHECK: result: sat
// CHECK: validate: checked={{[0-9]+}} mismatches=0
// CHECK: interp.earlier-version-loads: 1
// CHECK-NOT: interp.load-earlier-version
// INTERP: interp.load-earlier-version
!m = !sea.mem<!sea.addr<8>, i8>
sea.program @p {
  %m0 = sea.mem.nondet "m" [0] : !m
  %a = sea.segment "a" base 16 : i8 size 2 : i8 align 1 kind "stack" : !sea.addr<8>
  %b = sea.segment "b" base 32 : i8 size 1 : i8 align 1 kind "stack" : !sea.addr<8>
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %v inplace : !m, i8
  %x = sea.load %m0[%a] : !m -> i8
  %c = sea.const 3 : i8
  %ok = sea.icmp ne, %x, %c : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
