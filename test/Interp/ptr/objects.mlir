// RUN: sea-run %s --program=ok --missing=zero | FileCheck %s --check-prefix=OK
// RUN: not sea-run %s --program=oob 2>&1 | FileCheck %s --check-prefix=OOB
// RUN: not sea-run %s --program=null 2>&1 | FileCheck %s --check-prefix=NULL
// RUN: not sea-run %s --program=uaf 2>&1 | FileCheck %s --check-prefix=UAF
// RUN: not sea-run %s --program=double_free 2>&1 | FileCheck %s --check-prefix=DFREE
// RUN: not sea-run %s --program=bad_free 2>&1 | FileCheck %s --check-prefix=BADFREE
// RUN: sea-run %s --program=inputs --input=0@stack.1+2=42 | FileCheck %s --check-prefix=IN

!pm = !sea.mem<!sea.ptr<64>, i8>

// Pointers are (object, offset): a store through one pointer and a load
// through another into the same object meet; globals with one name are one
// object; pointers can be stored and loaded.
// OK: status: halted
sea.program @ok {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %two = sea.const 2 : i64
  %q = sea.ptr.add %p, %two : !sea.ptr<64>, i64
  %v = sea.const 9 : i8
  %m1 = sea.store %m0[%q], %v inplace : !pm, i8
  %r = sea.ptr.add %p, %two : !sea.ptr<64>, i64
  %x = sea.load %m1[%r] : !pm -> i8
  %a1 = sea.icmp eq, %x, %v : i8
  %g1 = sea.global @g size 8 align 8 : !sea.ptr<64>
  %g2 = sea.global @g size 8 align 8 : !sea.ptr<64>
  %a2 = sea.ptr.cmp eq, %g1, %g2 : !sea.ptr<64>
  %d = sea.ptr.diff %q, %p : !sea.ptr<64> -> i64
  %a3 = sea.icmp eq, %d, %two : i64
  %pm2 = sea.mem.nondet "ptrs" [2] : !sea.mem<!sea.ptr<64>, i8>
  %pm3 = sea.store %pm2[%g1], %q inplace : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64>
  %back = sea.load %pm3[%g1] : !sea.mem<!sea.ptr<64>, i8> -> !sea.ptr<64>
  %a4 = sea.ptr.cmp eq, %back, %q : !sea.ptr<64>
  %b1 = sea.and %a1, %a2 : i1
  %b2 = sea.and %a3, %a4 : i1
  %ok = sea.and %b1, %b2 : i1
  sea.assert %ok
  sea.halt
}

// OOB: message: memory access out of bounds: offset 4 in stack.1 of size 4
sea.program @oob {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %q = sea.ptr.add %p, %n : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] : !pm -> i8
  sea.halt
}

// NULL: message: memory access through the null pointer
sea.program @null {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %p = sea.ptr.null : !sea.ptr<64>
  %x = sea.load %m0[%p] : !pm -> i8
  sea.halt
}

// UAF: message: memory access to heap.1 after it was freed
sea.program @uaf {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 4 : i64
  %p = sea.malloc %n align 1 [1] : i64 -> !sea.ptr<64>
  sea.free %p : !sea.ptr<64>
  %x = sea.load %m0[%p] : !pm -> i8
  sea.halt
}

// DFREE: message: double free of heap.1
sea.program @double_free {
  %n = sea.const 4 : i64
  %p = sea.malloc %n align 1 [1] : i64 -> !sea.ptr<64>
  sea.free %p : !sea.ptr<64>
  sea.free %p : !sea.ptr<64>
  sea.halt
}

// BADFREE: message: free of a pointer that is not the start of a heap object (stack.1)
sea.program @bad_free {
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  sea.free %p : !sea.ptr<64>
  sea.halt
}

// Cells of abstract memories are named <object>+<offset>.
// IN: memread: site=0 addr=stack.1+2 width=8 value=42 origin=input
sea.program @inputs {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %two = sea.const 2 : i64
  %q = sea.ptr.add %p, %two : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] : !pm -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
