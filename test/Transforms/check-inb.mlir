// RUN: sea-opt %s --sea-check-inb 2>&1 | FileCheck %s
// RUN: sea-opt %s --sea-check-inb=static=false | FileCheck %s --check-prefix=NOSTATIC
// RUN: sea-opt %s --sea-check-inb=memhavoc-null-noop=false | FileCheck %s --check-prefix=STRICT
// RUN: sea-opt %s --sea-check-inb 2>&1 | FileCheck %s --check-prefix=WARN
// The interpreter agrees with the checks: an out-of-bounds access fails its
// check before it executes.
// RUN: sea-opt %s --sea-check-inb | sea-run --program=oob --input=2=16 - | FileCheck %s --check-prefix=RUN-OOB
// RUN: sea-opt %s --sea-check-inb | sea-run --program=oob --input=2=15 - | FileCheck %s --check-prefix=RUN-INB

!m = !sea.mem<!sea.ptr<64>, i8>

// A load and a store each get a check of their width; the program's assert
// goes, its assume stays.
// CHECK-LABEL: sea.program @access
// CHECK-SAME: sea.check_inb = {checks = 2 : i64, left = 2 : i64, static = 0 : i64
// CHECK: sea.assume
// CHECK: %[[W4:.*]] = sea.const 4 : i64
// CHECK: %[[D:.*]] = sea.ptr.dereferenceable %[[P:.*]], %[[W4]]
// CHECK: sea.assert %[[D]] {sea.inb_check = 0 : i64}
// CHECK-NEXT: sea.load
// CHECK: %[[W8:.*]] = sea.const 8 : i64
// CHECK: sea.ptr.dereferenceable %[[P]], %[[W8]]
// CHECK: sea.assert {{.*}} {sea.inb_check = 1 : i64}
// CHECK-NEXT: sea.store
// CHECK-NOT: sea.assert
// CHECK: sea.halt
sea.program @access {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [2] : i64
  %lt = sea.icmp ult, %n, %c16 : i64
  sea.assume %lt
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %x = sea.load %m0[%p] : !m -> i32
  %y = sea.const 1 : i64
  %m1 = sea.store %m0[%p], %y inplace : !m, i64
  %ok = sea.icmp eq, %x, %x : i32
  sea.assert %ok
  sea.halt
}

// Tier (a): constant offsets into objects of constant size. In bounds:
// removed (counted static). Out of bounds or not constant: kept.
// CHECK-LABEL: sea.program @tier_a
// CHECK-SAME: sea.check_inb = {checks = 9 : i64, left = 5 : i64, static = 4 : i64
// NOSTATIC-LABEL: sea.program @tier_a
// NOSTATIC-SAME: sea.check_inb = {checks = 9 : i64, left = 9 : i64, static = 0 : i64
sea.program @tier_a {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %g = sea.global @g size 8 align 8 : !sea.ptr<64>
  %c12 = sea.const 12 : i64
  %c13 = sea.const 13 : i64
  %one = sea.const 1 : i32
  // removed: a + 12, 4 bytes of 16
  // CHECK: sea.ptr.add
  // CHECK-NEXT: sea.load
  %p12 = sea.ptr.add %a, %c12 : !sea.ptr<64>, i64
  %x0 = sea.load %m0[%p12] : !m -> i32
  // kept: a + 13, 4 bytes run past the end
  // CHECK-NEXT: %[[O13:.*]] = sea.ptr.add
  // CHECK-NEXT: sea.const 4
  // CHECK-NEXT: sea.ptr.dereferenceable %[[O13]]
  // CHECK-NEXT: sea.assert
  // CHECK-NEXT: sea.load %{{.*}}[%[[O13]]]
  %p13 = sea.ptr.add %a, %c13 : !sea.ptr<64>, i64
  %x1 = sea.load %m0[%p13] : !m -> i32
  // removed: the global, through ptr.index with constant indices
  // (8 * 0 + 4)
  %z = sea.const 0 : i64
  %gi = sea.ptr.index %g [%z] strides [8] offset 4 inbounds : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%gi], %one inplace : !m, i32
  // kept: (8 * 1 + 4) is past the global's 8 bytes
  %o = sea.const 1 : i64
  %gj = sea.ptr.index %g [%o] strides [8] offset 4 inbounds : !sea.ptr<64>, i64
  %m2 = sea.store %m1[%gj], %one inplace : !m, i32
  // kept: a negative offset (a - 1) is a huge unsigned one
  %neg = sea.const -1 : i64
  %pm = sea.ptr.add %a, %neg : !sea.ptr<64>, i64
  %x2 = sea.load %m2[%pm] : !m -> i8
  // removed: a + 16 - 4, the steps summed modulo 2^64
  %c16b = sea.const 16 : i64
  %m4c = sea.const -4 : i64
  %q1 = sea.ptr.add %a, %c16b : !sea.ptr<64>, i64
  %q2 = sea.ptr.add %q1, %m4c : !sea.ptr<64>, i64
  %x3 = sea.load %m2[%q2] : !m -> i32
  // kept: a malloc of a symbolic size
  %n = sea.nondet "n" [2] : i64
  %h = sea.malloc %n align 8 [3] : i64 -> !sea.ptr<64>
  %x4 = sea.load %m2[%h] : !m -> i8
  // kept: a symbolic offset
  %k = sea.nondet "k" [4] : i64
  %pk = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %x5 = sea.load %m2[%pk] : !m -> i8
  // removed: the whole of a by memset
  %v = sea.const 0 : i8
  %m3 = sea.memset %m2[%a], %v, %c16 inplace : !m
  sea.halt
}

// Ranges. A constant length of 0 is no access: no check. A symbolic length
// n is checked as n == 0 || deref. memmove checks both ends; memhavoc of
// null is a no-op (null || n == 0 || deref) unless memhavoc-null-noop=false.
// CHECK-LABEL: sea.program @ranges
// CHECK-SAME: sea.check_inb = {checks = 4 : i64, left = 4 : i64, "null-noop" = 1 : i64, static = 0 : i64, "zero-length" = 1 : i64}
// CHECK: %[[Z:.*]] = sea.icmp eq, %[[N:.*]], %{{.*}} : i64
// CHECK: %[[OR:.*]] = sea.or %[[Z]], %{{.*}} : i1
// CHECK: sea.assert %[[OR]]
// CHECK-NEXT: sea.memset
// CHECK: sea.ptr.dereferenceable %[[DST:.*]], %[[N]]
// CHECK: sea.assert
// CHECK: sea.ptr.dereferenceable %[[SRC:.*]], %[[N]]
// CHECK: sea.assert
// CHECK-NEXT: sea.memmove %{{.*}}[%[[DST]]], %{{.*}}[%[[SRC]]]
// CHECK: sea.ptr.null
// CHECK: sea.ptr.cmp eq
// CHECK: sea.assert
// CHECK-NEXT: sea.memhavoc
// STRICT-LABEL: sea.program @ranges
// STRICT-NOT: sea.ptr.null
// STRICT: sea.memhavoc
sea.program @ranges {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.nondet "n" [1] : i64
  %s = sea.nondet "s" [2] : i64
  %p = sea.malloc %s align 8 [3] : i64 -> !sea.ptr<64>
  %q = sea.malloc %s align 8 [5] : i64 -> !sea.ptr<64>
  %v = sea.const 0 : i8
  %zero = sea.const 0 : i64
  %m1 = sea.memset %m0[%p], %v, %zero inplace : !m
  %m2 = sea.memset %m1[%p], %v, %n inplace : !m
  %m3 = sea.memmove %m2[%p], %m2[%q], %n inplace : !m
  %m4 = sea.memhavoc %m3[%p], %n [4] inplace : !m
  sea.halt
}

// A pointer from an integer, a function or a nondet pointer has an object
// of size 0: the check stays, and fails whenever the access executes.
// WARN: Warning: {{.*}}sea.ptr.from_int, an object of size 0{{.*}}[check-inb.size0-object]
// CHECK-LABEL: sea.program @int_to_ptr
// CHECK-SAME: left = 1 : i64
sea.program @int_to_ptr {
  %m0 = sea.mem.nondet "m" [0] : !m
  %i = sea.const 4096 : i64
  %p = sea.ptr.from_int %i : i64 to !sea.ptr<64>
  %x = sea.load %m0[%p] : !m -> i8
  sea.halt
}

// 32-bit pointers: tier (a) at the pointer width. a + 12 (4 of 16 bytes)
// is removed; a + 0xfffffffc, -4 modulo 2^32, is kept.
// CHECK-LABEL: sea.program @narrow
// CHECK-SAME: sea.check_inb = {checks = 2 : i64, left = 1 : i64, static = 1 : i64
// CHECK: sea.load
// CHECK-NEXT: sea.const -4 : i32
// CHECK-NEXT: %[[NEG:.*]] = sea.ptr.add %{{.*}}, %{{.*}} : !sea.ptr<32>, i32
// CHECK-NEXT: sea.const 4 : i32
// CHECK-NEXT: sea.ptr.dereferenceable %[[NEG]]
!m32 = !sea.mem<!sea.ptr<32>, i8>
sea.program @narrow {
  %m0 = sea.mem.nondet "m" [0] : !m32
  %c16 = sea.const 16 : i32
  %a = sea.alloca %c16 align 8 [1] : i32 -> !sea.ptr<32>
  %c12 = sea.const 12 : i32
  %p = sea.ptr.add %a, %c12 : !sea.ptr<32>, i32
  %x = sea.load %m0[%p] : !m32 -> i32
  %cm4 = sea.const -4 : i32
  %q = sea.ptr.add %a, %cm4 : !sea.ptr<32>, i32
  %y = sea.load %m0[%q] : !m32 -> i32
  sea.halt
}

// A global of size 0: no access of positive length is in it; kept.
// CHECK-LABEL: sea.program @size0_global
// CHECK-SAME: left = 1 : i64, static = 0 : i64
// CHECK: sea.ptr.dereferenceable
// CHECK: sea.assert
sea.program @size0_global {
  %m0 = sea.mem.nondet "m" [0] : !m
  %g = sea.global @empty size 0 align 1 : !sea.ptr<64>
  %x = sea.load %m0[%g] : !m -> i8
  sea.halt
}

// The interpreter runs the instrumented program: a + n, n = 16, fails its
// check; n = 15 passes.
// RUN-OOB: status: assert-failed
// RUN-INB: status: halted
sea.program @oob {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [2] : i64
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
