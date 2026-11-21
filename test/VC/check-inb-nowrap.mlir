// sea-vc --check-inb lists A-NOWRAP among the premises an unsat discharges
// only when every placed object is smaller than 2^(w-1) units, w the
// pointer width (docs/design/CheckInb.md, "A-INB and the premises"):
// A-NOWRAP reads an offset as signed, and an in-bounds offset of 2^(w-1)
// or more reads as negative.
// RUN: sea-vc %s --program=big --check-inb --lower --seam --ackermannize=off 2>&1 | FileCheck %s --check-prefix=BIG
// RUN: sea-vc %s --program=big_static --check-inb --lower --seam --ackermannize=off 2>&1 | FileCheck %s --check-prefix=BIG
// RUN: sea-vc %s --program=small --check-inb --lower --seam --ackermannize=off 2>&1 | FileCheck %s --check-prefix=SMALL

// BIG: Warning: {{.*}}2^31{{.*}}[check-inb.nowrap-not-implied]
// BIG: check-inb.discharges: a-inb a-sep a-null{{$}}
// BIG: result: unsat
// SMALL-NOT: check-inb.nowrap-not-implied
// SMALL: check-inb.discharges: a-inb a-sep a-null a-nowrap
// SMALL: result: unsat

!m = !sea.mem<!sea.ptr<32>, i8>

// An object of 2^31 units, read at a symbolic offset (one part).
sea.program @big {
  %m0 = sea.mem.nondet "m" [0] : !m
  %sz = sea.const 2147483648 : i32
  %a = sea.malloc %sz align 8 [1] : i32 -> !sea.ptr<32>
  %n = sea.nondet "n" [2] : i32
  %c16 = sea.const 16 : i32
  %lt = sea.icmp ult, %n, %c16 : i32
  sea.assume %lt
  %p = sea.ptr.add %a, %n : !sea.ptr<32>, i32
  %x = sea.load %m0[%p] : !m -> i8
  sea.halt
}

// The same object read at a constant offset: discharged statically, no
// part; the program's own sizes decide.
sea.program @big_static {
  %m0 = sea.mem.nondet "m" [0] : !m
  %sz = sea.const 2147483648 : i32
  %a = sea.malloc %sz align 8 [1] : i32 -> !sea.ptr<32>
  %x = sea.load %m0[%a] : !m -> i8
  sea.halt
}

// Every object small.
sea.program @small {
  %m0 = sea.mem.nondet "m" [0] : !m
  %sz = sea.const 16 : i32
  %a = sea.malloc %sz align 8 [1] : i32 -> !sea.ptr<32>
  %n = sea.nondet "n" [2] : i32
  %lt = sea.icmp ult, %n, %sz : i32
  sea.assume %lt
  %p = sea.ptr.add %a, %n : !sea.ptr<32>, i32
  %x = sea.load %m0[%p] : !m -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
