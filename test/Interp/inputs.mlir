// RUN: sea-run %s --input=0=255,1=-128,2=0x7fff,3=18446744073709551616 | FileCheck %s
// RUN: not sea-run %s --input=0=256,1=0,2=0,3=0 2>&1 | FileCheck %s --check-prefix=WIDE
// RUN: not sea-run %s --input=0=0,1=-129,2=0,3=0 2>&1 | FileCheck %s --check-prefix=NEG
// RUN: not sea-run %s --input=0=0,1=0,2=0x,3=0 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not sea-run %s --input=x=1 2>&1 | FileCheck %s --check-prefix=SITE
// RUN: sea-run %s --missing=random --seed=1 > %t.1
// RUN: sea-run %s --missing=random --seed=1 > %t.2
// RUN: diff %t.1 %t.2

// CHECK: site=0 callee=nondet_u8 width=8 value=255 origin=input
// CHECK: site=1 callee=nondet_i8 width=8 value=128 origin=input
// CHECK: site=2 callee=nondet_u16 width=16 value=32767 origin=input
// CHECK: site=3 callee=nondet_u128 width=128 value=18446744073709551616 origin=input
// CHECK: status: halted
// WIDE: message: input '256' for site 0 is not an i8 value
// NEG: message: input '-129' for site 1 is not an i8 value
// BAD: message: input '0x' for site 2 is not an i16 value
// SITE: bad --input 'x=1'
sea.program @main {
  %a = sea.nondet "nondet_u8" [0] : i8
  %b = sea.nondet "nondet_i8" [1] : i8
  %c = sea.nondet "nondet_u16" [2] : i16
  %d = sea.nondet "nondet_u128" [3] : i128
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
