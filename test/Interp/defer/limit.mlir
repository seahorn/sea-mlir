// Limits and layout errors are never deferred: with --defer-memory-errors
// the run still stops with status error (exit 1). A limit after a deferred
// violation leaves the run unclassified: status error, with the count.
// RUN: not sea-run %s --program=to_int --defer-memory-errors 2>&1 | FileCheck %s --check-prefix=TOINT
// RUN: not sea-run %s --program=havoc_limit --defer-memory-errors 2>&1 | FileCheck %s --check-prefix=HAVOC
// RUN: not sea-run %s --program=misaligned --defer-memory-errors 2>&1 | FileCheck %s --check-prefix=MIS
// RUN: not sea-run %s --program=after --defer-memory-errors 2>&1 | FileCheck %s --check-prefix=AFTER

// TOINT: status: error
// TOINT-NOT: deferred:
// TOINT: message: ptr.to_int: pointers have no address before placement
sea.program @to_int {
  %n = sea.const 1 : i64
  %p = sea.alloca %n align 1 [0] : i64 -> !sea.ptr<64>
  %i = sea.ptr.to_int %p : !sea.ptr<64> to i64
  sea.halt
}

// The memhavoc length limit, in bounds of a large object.
// HAVOC: status: error
// HAVOC-NOT: deferred:
// HAVOC: message: memhavoc of {{[0-9]+}} cells exceeds the interpreter limit
sea.program @havoc_limit {
  %m0 = sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i8>
  %n = sea.const 0x10000000000 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %m1 = sea.memhavoc %m0[%p], %n [2] inplace : !sea.mem<!sea.ptr<64>, i8>
  sea.halt
}

// MIS: status: error
// MIS-NOT: deferred:
// MIS: message: aligned 4-byte word layout: access at stack.1+2 is not word-aligned
sea.program @misaligned {
  %m0 = sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i32, aligned>
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %two = sea.const 2 : i64
  %q = sea.ptr.add %p, %two : !sea.ptr<64>, i64
  %v = sea.load %m0[%q] : !sea.mem<!sea.ptr<64>, i32, aligned> -> i32
  sea.halt
}

// AFTER: status: error
// AFTER-NEXT: at: loc({{.*}}limit.mlir":[[@LINE+9]]
// AFTER-NEXT: message: ptr.to_int
// AFTER-NEXT: deferred: 1 kind=null at={{.*}}limit.mlir:[[@LINE+4]]
sea.program @after {
  %m0 = sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i8>
  %null = sea.ptr.null : !sea.ptr<64>
  %v = sea.load %m0[%null] : !sea.mem<!sea.ptr<64>, i8> -> i8
  %n = sea.const 1 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %i = sea.ptr.to_int %p : !sea.ptr<64> to i64
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
