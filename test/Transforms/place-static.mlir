// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static))' -split-input-file 2>/dev/null | FileCheck %s
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static))' -split-input-file 2>&1 >/dev/null | FileCheck %s --check-prefix=DIAG
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static{symbolic-size=assert}))' -split-input-file 2>/dev/null | FileCheck %s --check-prefix=ASSERT
// RUN: not sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static{symbolic-size=refuse}))' -split-input-file 2>&1 | FileCheck %s --check-prefix=REFUSE

// SeaHorn's layout: functions, globals, heap upwards from 0x08048000; stack
// downwards from 0xC0000000. Pointers become addresses everywhere.
// CHECK-LABEL: sea.program @layout
// CHECK-DAG: sea.segment "func.main" base 134512640 : i64 size 4 : i64 align 4 kind "func"
// CHECK-DAG: sea.segment "global.g" base 134512644 : i64 size 4 : i64 align 4 kind "global"
// CHECK-DAG: sea.segment "heap.2" base 134512656 : i64 size 32 : i64 align 16 kind "heap"
// CHECK-DAG: sea.segment "stack.1" base 3221225464 : i64 size 8 : i64 align 8 kind "stack"
// CHECK-NOT: !sea.ptr
// CHECK: sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
// CHECK: sea.addr.add
// CHECK: sea.addr.cmp eq
// CHECK-NOT: sea.free
sea.program @layout {
  %m = sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i8>
  %n8 = sea.const 8 : i64
  %n32 = sea.const 32 : i64
  %s = sea.alloca %n8 align 8 [1] : i64 -> !sea.ptr<64>
  %h = sea.malloc %n32 align 16 [2] : i64 -> !sea.ptr<64>
  %g = sea.global @g size 4 align 4 : !sea.ptr<64>
  %f = sea.func_addr @main : !sea.ptr<64>
  %one = sea.const 1 : i64
  %q = sea.ptr.add %s, %one : !sea.ptr<64>, i64
  %null = sea.ptr.null : !sea.ptr<64>
  %c = sea.ptr.cmp eq, %q, %null : !sea.ptr<64>
  sea.free %h : !sea.ptr<64>
  sea.halt
}

// -----

// A non-constant size gets 4096 units and an explicit bound.
// CHECK-LABEL: sea.program @symbolic
// DIAG: Warning: an allocation of non-constant size gets 4096 units; its size is assumed to fit
// CHECK: sea.icmp ule
// CHECK-NEXT: sea.assume
// ASSERT-LABEL: sea.program @symbolic
// ASSERT: sea.icmp ule
// ASSERT-NEXT: sea.assert
// REFUSE: Error: an allocation of non-constant size (symbolic-size=refuse)
sea.program @symbolic {
  %k = sea.nondet "nd" [0] : i64
  %h = sea.malloc %k align 8 [1] : i64 -> !sea.ptr<64>
  sea.halt
}

// -----

// Pointers too narrow for SeaHorn's layout: compact placement from 1 (0
// stays null), with a warning.
// DIAG: Warning: pointers of 8 bits are too narrow for SeaHorn's layout
// CHECK-LABEL: sea.program @narrow
// CHECK: sea.segment "stack.0" base 1 : i8 size 2 : i8 align 1 kind "stack"
// CHECK: sea.segment "stack.1" base 4 : i8 size 4 : i8 align 4 kind "stack"
sea.program @narrow {
  %n2 = sea.const 2 : i8
  %n4 = sea.const 4 : i8
  %a = sea.alloca %n2 align 1 [0] : i8 -> !sea.ptr<8>
  %b = sea.alloca %n4 align 4 [1] : i8 -> !sea.ptr<8>
  sea.halt
}

// -----

// An object's uses become its segment's result (plus addr.add for offsets),
// not a numeral: the object stays visible after placement (SeaM). One
// segment per object, however often it is named. Null is an object too
// (docs/design/SeaM-null.md): the segment "null", base 0, size 0, first.
// CHECK-LABEL: sea.program @provenance
// CHECK: %[[N:.*]] = sea.segment "null" base 0 : i64 size 0 : i64 align {{[0-9]+}} kind "null" : !sea.addr<64>
// CHECK-DAG: %[[G:.*]] = sea.segment "global.g" {{.*}} : !sea.addr<64>
// CHECK-DAG: %[[S:.*]] = sea.segment "stack.1" {{.*}} : !sea.addr<64>
// CHECK-NOT: sea.segment "global.g"
// CHECK: sea.addr.add %[[S]],
// CHECK: sea.addr.cmp eq, %[[G]], %[[G]]
// CHECK-NOT: sea.addr.from_int
// CHECK: sea.addr.cmp eq, %[[N]],
sea.program @provenance {
  %n8 = sea.const 8 : i64
  %s = sea.alloca %n8 align 8 [1] : i64 -> !sea.ptr<64>
  %one = sea.const 1 : i64
  %q = sea.ptr.add %s, %one : !sea.ptr<64>, i64
  %g1 = sea.global @g size 4 align 4 : !sea.ptr<64>
  %g2 = sea.global @g size 4 align 4 : !sea.ptr<64>
  %c = sea.ptr.cmp eq, %g1, %g2 : !sea.ptr<64>
  %null = sea.ptr.null : !sea.ptr<64>
  %d = sea.ptr.cmp eq, %null, %q : !sea.ptr<64>
  sea.halt
}

// -----

// Null provenance comes only from sea.ptr.null (SeaM-null.md, point 7): an
// integer 0 cast to a pointer stays a number, and a program with no
// sea.ptr.null has no null segment.
// CHECK-LABEL: sea.program @no_null
// CHECK-NOT: sea.segment "null"
// CHECK: %[[Z:.*]] = sea.const 0 : i64
// CHECK: sea.addr.from_int %[[Z]]
sea.program @no_null {
  %z = sea.const 0 : i64
  %p = sea.ptr.from_int %z : i64 to !sea.ptr<64>
  %n8 = sea.const 8 : i64
  %s = sea.alloca %n8 align 8 [1] : i64 -> !sea.ptr<64>
  %d = sea.ptr.cmp eq, %p, %s : !sea.ptr<64>
  sea.halt
}

// -----

// Compact layout (narrow pointers): null at 0, the first object at 1.
// CHECK-LABEL: sea.program @null_compact
// CHECK: sea.segment "null" base 0 : i8 size 0 : i8 align {{[0-9]+}} kind "null"
// CHECK: sea.segment "stack.0" base 1 : i8
sea.program @null_compact {
  %n2 = sea.const 2 : i8
  %a = sea.alloca %n2 align 1 [0] : i8 -> !sea.ptr<8>
  %null = sea.ptr.null : !sea.ptr<8>
  %d = sea.ptr.cmp eq, %null, %a : !sea.ptr<8>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
