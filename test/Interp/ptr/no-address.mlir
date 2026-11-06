// RUN: not sea-run %s --program=to_int 2>&1 | FileCheck %s --check-prefix=TOINT
// RUN: not sea-run %s --program=from_int 2>&1 | FileCheck %s --check-prefix=FROMINT
// RUN: sea-run %s --program=null_from_int | FileCheck %s --check-prefix=NULLOK
// RUN: not sea-run %s --program=punning 2>&1 | FileCheck %s --check-prefix=PUN
// RUN: not sea-run %s --program=ordered 2>&1 | FileCheck %s --check-prefix=ORD

// Before placement, pointers have no address.
// TOINT: message: ptr.to_int: pointers have no address before placement
sea.program @to_int {
  %n = sea.const 1 : i64
  %p = sea.alloca %n align 1 [0] : i64 -> !sea.ptr<64>
  %i = sea.ptr.to_int %p : !sea.ptr<64> to i64
  sea.halt
}

// FROMINT: message: ptr.from_int of a non-zero value
sea.program @from_int {
  %c = sea.const 8 : i64
  %p = sea.ptr.from_int %c : i64 to !sea.ptr<64>
  sea.halt
}

// NULLOK: status: halted
sea.program @null_from_int {
  %z = sea.const 0 : i64
  %p = sea.ptr.from_int %z : i64 to !sea.ptr<64>
  %n = sea.ptr.null : !sea.ptr<64>
  %ok = sea.ptr.cmp eq, %p, %n : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}

// PUN: message: load of a pointer cell as an integer
sea.program @punning {
  %m0 = sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i8>
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %m1 = sea.store %m0[%p], %p inplace : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64>
  %i = sea.load %m1[%p] : !sea.mem<!sea.ptr<64>, i8> -> i64
  sea.halt
}

// ORD: message: ordered ptr.cmp of pointers into different objects
sea.program @ordered {
  %n = sea.const 1 : i64
  %p = sea.alloca %n align 1 [0] : i64 -> !sea.ptr<64>
  %q = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %lt = sea.ptr.cmp ult, %p, %q : !sea.ptr<64>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
