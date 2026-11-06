// RUN: sea-run %s --program=deref --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-run %s --program=base --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s | sea-opt | FileCheck %s --check-prefix=RT
// RUN: not sea-opt %s --sea-place-static 2>&1 | FileCheck %s --check-prefix=PLACE

// sea.ptr.dereferenceable / sea.ptr.base: object metadata of abstract
// pointers (spatial; the fat-pointer lowering is layer 3).

// RT: sea.ptr.dereferenceable %{{.*}}, %{{.*}} : !sea.ptr<64>, i64
// RT: sea.ptr.base %{{.*}} : !sea.ptr<64>
// PLACE: needs the fat-pointer / shadow-memory lowering{{.*}}[place.fat-pointer-op]

// OK: status: halted
sea.program @deref {
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [0] : i64 -> !sea.ptr<64>
  %zero = sea.const 0 : i64
  %one = sea.const 1 : i64
  %six = sea.const 6 : i64
  %nine = sea.const 9 : i64
  %q = sea.ptr.add %p, %six : !sea.ptr<64>, i64
  %end = sea.ptr.add %p, %n : !sea.ptr<64>, i64
  %neg = sea.const -1 : i64
  %before = sea.ptr.add %p, %neg : !sea.ptr<64>, i64
  %null = sea.ptr.null : !sea.ptr<64>
  %two = sea.const 2 : i64
  %three = sea.const 3 : i64
  // [p, p+8) yes; [p, p+9) no
  %a = sea.ptr.dereferenceable %p, %n : !sea.ptr<64>, i64
  %b = sea.ptr.dereferenceable %p, %nine : !sea.ptr<64>, i64
  // [p+6, p+8) yes; [p+6, p+9) no
  %c = sea.ptr.dereferenceable %q, %two : !sea.ptr<64>, i64
  %d = sea.ptr.dereferenceable %q, %three : !sea.ptr<64>, i64
  // one past the end: 0 bytes yes, 1 byte no
  %e = sea.ptr.dereferenceable %end, %zero : !sea.ptr<64>, i64
  %f = sea.ptr.dereferenceable %end, %one : !sea.ptr<64>, i64
  // before the start (offset wraps): no; null: no
  %g = sea.ptr.dereferenceable %before, %one : !sea.ptr<64>, i64
  %h = sea.ptr.dereferenceable %null, %zero : !sea.ptr<64>, i64
  // a huge length must not wrap around
  %i = sea.ptr.dereferenceable %q, %neg : !sea.ptr<64>, i64
  %t = sea.const 1 : i1
  %f0 = sea.const 0 : i1
  %ok1 = sea.icmp eq, %a, %t : i1
  %ok2 = sea.icmp eq, %b, %f0 : i1
  %ok3 = sea.icmp eq, %c, %t : i1
  %ok4 = sea.icmp eq, %d, %f0 : i1
  %ok5 = sea.icmp eq, %e, %t : i1
  %ok6 = sea.icmp eq, %f, %f0 : i1
  %ok7 = sea.icmp eq, %g, %f0 : i1
  %ok8 = sea.icmp eq, %h, %f0 : i1
  %ok9 = sea.icmp eq, %i, %f0 : i1
  %r1 = sea.and %ok1, %ok2 : i1
  %r2 = sea.and %r1, %ok3 : i1
  %r3 = sea.and %r2, %ok4 : i1
  %r4 = sea.and %r3, %ok5 : i1
  %r5 = sea.and %r4, %ok6 : i1
  %r6 = sea.and %r5, %ok7 : i1
  %r7 = sea.and %r6, %ok8 : i1
  %r8 = sea.and %r7, %ok9 : i1
  sea.assert %r8
  sea.halt
}

// The base is the same object at offset 0; the base of null is null.
sea.program @base {
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [0] : i64 -> !sea.ptr<64>
  %five = sea.const 5 : i64
  %q = sea.ptr.add %p, %five : !sea.ptr<64>, i64
  %b = sea.ptr.base %q : !sea.ptr<64>
  %a1 = sea.ptr.cmp eq, %b, %p : !sea.ptr<64>
  %null = sea.ptr.null : !sea.ptr<64>
  %bn = sea.ptr.base %null : !sea.ptr<64>
  %a2 = sea.ptr.cmp eq, %bn, %null : !sea.ptr<64>
  %ok = sea.and %a1, %a2 : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
