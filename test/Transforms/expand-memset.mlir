// RUN: sea-run %s --input=1=2 | FileCheck %s --check-prefix=SET2
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-expand-memset{max-length=3}))' | sea-run - --input=1=2 | FileCheck %s --check-prefix=SET2
// RUN: sea-run %s --input=1=4 | FileCheck %s --check-prefix=SET4
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-expand-memset{max-length=3}))' | sea-run - --input=1=4 | FileCheck %s --check-prefix=BOUND
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-expand-memset{max-length=3}))' 2>&1 | FileCheck %s --check-prefix=WARN
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VC

// memset of n cells at 16, then the cell at 18 is checked: it is set iff
// n >= 3. The expansion is exact up to max-length; beyond it the run is
// assumed away (and the bound is reported).
// SET2: status: halted
// SET4: status: assert-failed
// BOUND: status: assume-blocked
// WARN: memset.symbolic-bound
// VC: result: sat
// VC: replay: assert-failed
sea.program @main {
  %m0 = sea.mem.nondet "r" [0] : !sea.mem<!sea.addr<8>, i8>
  %n = sea.nondet "nondet_u8" [1] : i8
  %p = sea.const 16 : i8
  %pa = sea.addr.from_int %p : i8 to !sea.addr<8>
  %v = sea.const 7 : i8
  %w = sea.const 9 : i8
  %q = sea.const 18 : i8
  %qa = sea.addr.from_int %q : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%qa], %w inplace : !sea.mem<!sea.addr<8>, i8>, i8
  %m2 = sea.memset %m1[%pa], %v, %n inplace : !sea.mem<!sea.addr<8>, i8>
  %x = sea.load %m2[%qa] : !sea.mem<!sea.addr<8>, i8> -> i8
  %ok = sea.icmp ne, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
