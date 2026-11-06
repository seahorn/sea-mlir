// RUN: sea-run %s --program=safe --input=0=5 | FileCheck %s --check-prefix=SAFE
// RUN: sea-run %s --program=safe --input=0=50 | FileCheck %s --check-prefix=BLOCKED
// RUN: sea-run %s --program=bug --input=0=9 | FileCheck %s --check-prefix=BUG
// RUN: sea-run %s --program=bug --input=0=3 | FileCheck %s --check-prefix=NOBUG
// RUN: not sea-run %s --program=bug 2>&1 | FileCheck %s --check-prefix=MISSING
// RUN: sea-run %s --program=bug --missing=zero | FileCheck %s --check-prefix=ZERO
// RUN: not sea-run %s 2>&1 | FileCheck %s --check-prefix=SEVERAL

// SAFE: program: @safe
// SAFE-NEXT: nondet: site=0 callee=nondet_u32 width=32 value=5 origin=input
// SAFE-NEXT: status: halted
// BLOCKED: status: assume-blocked
// BLOCKED-NEXT: at: loc({{.*}}status.mlir":[[@LINE+5]]
sea.program @safe {
  %x = sea.nondet "nondet_u32" [0] : i32
  %c10 = sea.const 10 : i32
  %lt = sea.icmp ult, %x, %c10 : i32
  sea.assume %lt
  %y = sea.add %x, %x : i32
  %c20 = sea.const 20 : i32
  %ok = sea.icmp ult, %y, %c20 : i32
  sea.assert %ok
  sea.halt
}

// BUG: status: assert-failed
// BUG-NEXT: at: loc({{.*}}status.mlir":[[@LINE+9]]
// NOBUG: status: halted
// MISSING: message: no input for nondet site 0
// ZERO: value=0 origin=zero
// ZERO: status: halted
sea.program @bug {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c8 = sea.const 8 : i8
  %ok = sea.icmp ule, %x, %c8 : i8
  sea.assert %ok
  sea.halt
}

// SEVERAL: several programs; pick one with --program

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
