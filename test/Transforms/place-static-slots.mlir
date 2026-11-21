// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static{slots=aligned}))' -split-input-file 2>/dev/null | FileCheck %s
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static{slots=aligned}))' -split-input-file 2>&1 >/dev/null | FileCheck %s --check-prefix=DIAG
// RUN: not sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static{slots=wide}))' -split-input-file 2>&1 | FileCheck %s --check-prefix=BAD

// slots=aligned: each object owns [base, base + 2^s), 2^s > max(room, 1),
// base a multiple of max(2^s, align); same order and windows as without
// slots (func, global, heap up from 0x08048000; stack down from
// 0xC0000000); null at 0.
// CHECK-LABEL: sea.program @sizes
// CHECK-SAME: sea.place_stats = {objects = 12 : i64, "over-aligned" = {{[0-9]+}} : i64, "slot-bits-max" = 21 : i64, span = {{[0-9]+}} : i64}
// CHECK-DAG: sea.segment "null" base 0 : i64 size 0 : i64
// room 4 -> s 3: 0x08048000
// CHECK-DAG: sea.segment "func.main" base 134512640 : i64 size 4 : i64 align 4 kind "func"
// size 0 -> one unit -> s 1: 0x08048008
// CHECK-DAG: sea.segment "global.zero" base 134512648 : i64 size 0 : i64 align 1 kind "global"
// size 1 -> s 1: 0x0804800a
// CHECK-DAG: sea.segment "global.one" base 134512650 : i64 size 1 : i64 align 1 kind "global"
// align 64 above the slot (s 3): 0x08048040 (the next base is aligned to
// 64 as without slots)
// CHECK-DAG: sea.segment "global.big_align" base 134512704 : i64 size 4 : i64 align 64 kind "global"
// size 16 = 2^4 -> s 5 (the end address stays in the slot): 0x08048080
// CHECK-DAG: sea.segment "heap.2" base 134512768 : i64 size 16 : i64 align 4 kind "heap"
// size 15 = 2^4 - 1 -> s 4: 0x080480a0
// CHECK-DAG: sea.segment "heap.3" base 134512800 : i64 size 15 : i64 align 1 kind "heap"
// size 17 -> s 5: 0x080480c0
// CHECK-DAG: sea.segment "heap.4" base 134512832 : i64 size 17 : i64 align 1 kind "heap"
// symbolic size: the room 4096 -> s 13: 0x0804a000
// CHECK-DAG: sea.segment "heap.5" base 134520832 : i64 size 4096 : i64 align 8 kind "heap"
// a nondet pointer: room 1 -> s 1: 0x0804c000
// CHECK-DAG: sea.segment "nondet.6" base 134529024 : i64 size 1 : i64 align 1 kind "heap"
// stack downwards: size 8 -> s 4: 0xbffffff0; size 5 -> s 3: 0xbfffffe8;
// size 2^20 -> s 21: 0xbfc00000
// CHECK-DAG: sea.segment "stack.1" base 3221225456 : i64 size 8 : i64 align 8 kind "stack"
// CHECK-DAG: sea.segment "stack.7" base 3221225448 : i64 size 5 : i64 align 4 kind "stack"
// CHECK-DAG: sea.segment "stack.8" base 3217031168 : i64 size 1048576 : i64 align 1 kind "stack"
// DIAG-NOT: place.slots-fallback
// BAD: sea-place-static: slots must be none or aligned
sea.program @sizes {
  %n0 = sea.const 0 : i64
  %n5 = sea.const 5 : i64
  %n8 = sea.const 8 : i64
  %n15 = sea.const 15 : i64
  %n16 = sea.const 16 : i64
  %n17 = sea.const 17 : i64
  %n1m = sea.const 1048576 : i64
  %s = sea.alloca %n8 align 8 [1] : i64 -> !sea.ptr<64>
  %h2 = sea.malloc %n16 align 4 [2] : i64 -> !sea.ptr<64>
  %h3 = sea.malloc %n15 align 1 [3] : i64 -> !sea.ptr<64>
  %h4 = sea.malloc %n17 align 1 [4] : i64 -> !sea.ptr<64>
  %k = sea.nondet "k" [9] : i64
  %h5 = sea.malloc %k align 8 [5] : i64 -> !sea.ptr<64>
  %p = sea.ptr.nondet "p" [6] : !sea.ptr<64>
  %s7 = sea.alloca %n5 align 4 [7] : i64 -> !sea.ptr<64>
  %s8 = sea.alloca %n1m align 1 [8] : i64 -> !sea.ptr<64>
  %z = sea.global @zero size 0 align 1 : !sea.ptr<64>
  %o = sea.global @one size 1 align 1 : !sea.ptr<64>
  %b = sea.global @big_align size 4 align 64 : !sea.ptr<64>
  %f = sea.func_addr @main : !sea.ptr<64>
  %null = sea.ptr.null : !sea.ptr<64>
  sea.halt
}

// -----

// The slots do not fit the stack window (9 MiB below 0xC0000000): 5 MiB
// takes an 8 MiB slot, 3 MiB a 4 MiB one. Every object is placed without
// slots instead, with a warning.
// CHECK-LABEL: sea.program @stack_window
// CHECK-DAG: sea.segment "stack.1" base 3215982592 : i64 size 5242880 : i64 align 1 kind "stack"
// CHECK-DAG: sea.segment "stack.2" base 3212836864 : i64 size 3145728 : i64 align 1 kind "stack"
// DIAG: Warning: sea-place-static: no aligned slot for stack.2 (3145728 units, align 1) in the seahorn layout of a 64-bit address space; placing every object without slots
// DIAG-SAME: [place.slots-fallback]
sea.program @stack_window {
  %a = sea.const 5242880 : i64
  %b = sea.const 3145728 : i64
  %s1 = sea.alloca %a align 1 [1] : i64 -> !sea.ptr<64>
  %s2 = sea.alloca %b align 1 [2] : i64 -> !sea.ptr<64>
  sea.halt
}

// -----

// 8-bit pointers (compact layout, upwards from 1): 100 units take a slot of
// 128 at 128, and 50 more do not fit below 256. Without slots: 1 and 101.
// CHECK-LABEL: sea.program @narrow_width
// CHECK-DAG: sea.segment "heap.1" base 1 : i8 size 100 : i8 align 1 kind "heap"
// CHECK-DAG: sea.segment "heap.2" base 101 : i8 size 50 : i8 align 1 kind "heap"
// (the second fallback of this file: in the warning's summary)
// DIAG: Warning: [place.slots-fallback] occurred 2 times
sea.program @narrow_width {
  %a = sea.const 100 : i8
  %b = sea.const 50 : i8
  %h1 = sea.malloc %a align 1 [1] : i8 -> !sea.ptr<8>
  %h2 = sea.malloc %b align 1 [2] : i8 -> !sea.ptr<8>
  sea.halt
}

// -----

// The compact layout with room for the slots: 1 unit at 2, 3 units at 4
// (s 2), 4 units at 8 (s 3).
// CHECK-LABEL: sea.program @compact
// CHECK-DAG: sea.segment "heap.1" base 2 : i16 size 1 : i16 align 1 kind "heap"
// CHECK-DAG: sea.segment "heap.2" base 4 : i16 size 3 : i16 align 1 kind "heap"
// CHECK-DAG: sea.segment "heap.3" base 8 : i16 size 4 : i16 align 2 kind "heap"
sea.program @compact {
  %n1 = sea.const 1 : i16
  %n3 = sea.const 3 : i16
  %n4 = sea.const 4 : i16
  %h1 = sea.malloc %n1 align 1 [1] : i16 -> !sea.ptr<16>
  %h2 = sea.malloc %n3 align 1 [2] : i16 -> !sea.ptr<16>
  %h3 = sea.malloc %n4 align 2 [3] : i16 -> !sea.ptr<16>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
