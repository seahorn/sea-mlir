// The substitute effects of deferred violations: an update (store, memset,
// memmove, memhavoc) with a violation does nothing, so the memory keeps its
// contents; a bad free does nothing; a load reads 0 or the null pointer.
// (The memhavoc of null is a violation under --memhavoc-null=error; by
// default it is a no-op, test/Interp/ptr/memhavoc-null.test.)
// Every violation is counted, the first is reported.
// RUN: not sea-run %s --program=updates --defer-memory-errors --memhavoc-null=error | FileCheck %s --check-prefix=UPD
// RUN: not sea-run %s --program=updates --defer-memory-errors | FileCheck %s --check-prefix=UPD0
// RUN: not sea-run %s --program=frees --defer-memory-errors | FileCheck %s --check-prefix=FREE
// RUN: not sea-run %s --program=ptr_load --defer-memory-errors | FileCheck %s --check-prefix=PTR
// RUN: not sea-run %s --program=move_src --defer-memory-errors | FileCheck %s --check-prefix=MOVE
// RUN: not sea-run %s --program=move_src 2>&1 | FileCheck %s --check-prefix=MOVESTOP

// The assert holds (the in-bounds byte is intact); the run is a finding at
// the first violation.
// UPD: status: memory-error
// UPD-NEXT: at: loc({{.*}}updates.mlir":[[@LINE+20]]
// UPD-NEXT: reached: halted
// UPD-NEXT: message: memory access out of bounds: offset 4 in stack.1 of size 4
// UPD-NEXT: deferred: 4 kind=out-of-bounds
// By default the memhavoc of null is a no-op: one violation fewer.
// UPD0: status: memory-error
// UPD0: deferred: 3 kind=out-of-bounds
// UPD0: assume.memhavoc-null-noop: uses=1
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @updates {
  %z0 = sea.const 0 : i8
  %m0 = sea.mem.const %z0 : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %three = sea.const 3 : i64
  %q = sea.ptr.add %p, %three : !sea.ptr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%q], %v inplace : !pm, i8
  // a 2-byte store from offset 3 straddles the end: dropped whole
  %w = sea.const 0x0909 : i16
  %m2 = sea.store %m1[%q], %w inplace : !pm, i16
  %two = sea.const 2 : i64
  %b = sea.const 9 : i8
  %m3 = sea.memset %m2[%q], %b, %two inplace : !pm
  %m4 = sea.memmove %m3[%q], %m3[%p], %two inplace : !pm
  %null = sea.ptr.null : !sea.ptr<64>
  %m5 = sea.memhavoc %m4[%null], %two [2] inplace : !pm
  %x = sea.load %m5[%q] : !pm -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// FREE: status: memory-error
// FREE-NEXT: at: loc({{.*}}updates.mlir":[[@LINE+10]]
// FREE-NEXT: reached: halted
// FREE-NEXT: message: double free of heap.1
// FREE-NEXT: deferred: 2 kind=double-free
sea.program @frees {
  %n = sea.const 4 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %one = sea.const 1 : i64
  %q = sea.ptr.add %p, %one : !sea.ptr<64>, i64
  sea.free %p : !sea.ptr<64>
  sea.free %p : !sea.ptr<64>
  sea.free %q : !sea.ptr<64>
  sea.halt
}

// A pointer loaded past a violation is null: an access through it is
// another violation.
// PTR: status: memory-error
// PTR: message: memory access out of bounds
// PTR-NEXT: deferred: 2 kind=out-of-bounds
sea.program @ptr_load {
  %z0 = sea.const 0 : i8
  %m0 = sea.mem.const %z0 : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %r = sea.load %m0[%p] : !pm -> !sea.ptr<64>
  %x = sea.load %m0[%r] : !pm -> i8
  %z = sea.const 0 : i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// A memmove whose source leaves its object does nothing: the destination
// keeps its contents (the assert holds), and the run is a finding at it.
// MOVE: status: memory-error
// MOVE-NEXT: at: loc({{.*}}updates.mlir":[[@LINE+14]]
// MOVE-NEXT: reached: halted
// MOVE-NEXT: message: memory access out of bounds: offset 4 in stack.1 of size 4
// MOVE-NEXT: deferred: 1 kind=out-of-bounds
// MOVESTOP: status: error
// MOVESTOP: message: memory access out of bounds: offset 4 in stack.1 of size 4
sea.program @move_src {
  %z0 = sea.const 0 : i8
  %m0 = sea.mem.const %z0 : !pm
  %n = sea.const 4 : i64
  %src = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %dst = sea.alloca %n align 1 [2] : i64 -> !sea.ptr<64>
  %two = sea.const 2 : i64
  %q = sea.ptr.add %src, %two : !sea.ptr<64>, i64
  %m1 = sea.memmove %m0[%dst], %m0[%q], %n inplace : !pm
  %x = sea.load %m1[%dst] : !pm -> i8
  %ok = sea.icmp eq, %x, %z0 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
