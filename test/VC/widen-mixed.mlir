// Several word sizes in one program (sea-vc --widen-memory=8, per-class
// sizes): memory a is accessed by 8-byte words, b by 4, c by 2, d by bytes;
// each gets its own cell. Every VC encoding takes the mix: @unsat (a store
// then a load at the same index) stays unsat, and @sat's counterexample
// replays (validate: mismatches=0), in the program as lowered and in the
// original one. With --widen-memory-per-class=false only a is widened.
// RUN: sea-vc %s --program=unsat --lower --widen-memory=8 2>&1 | FileCheck %s --check-prefixes=SIZES,UNSAT
// RUN: sea-vc %s --program=unsat --lower --widen-memory=8 --widen-memory-per-class=false 2>&1 | FileCheck %s --check-prefixes=FIXED,UNSAT
// RUN: sea-vc %s --program=unsat --lower --widen-memory=4 2>&1 | FileCheck %s --check-prefixes=MAX4,UNSAT
// RUN: sea-vc %s --program=unsat --lower --widen-memory=8 --seam --seam-check 2>&1 | FileCheck %s --check-prefixes=UNSAT,CHECKED
// RUN: sea-vc %s --program=unsat --lower --widen-memory=8 --seam --seam-wide 2>&1 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=unsat --lower --widen-memory=8 --mem=lambda 2>&1 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=sat --lower --widen-memory=8 --replay-original 2>&1 | FileCheck %s --check-prefixes=SAT,ORIG
// RUN: sea-vc %s --program=sat --lower --widen-memory=8 --seam --seam-check 2>&1 | FileCheck %s --check-prefixes=SAT,CHECKED
// RUN: sea-vc %s --program=sat --lower --widen-memory=8 --seam --seam-wide --seam-words=auto --words-check 2>&1 | FileCheck %s --check-prefixes=SAT,WORDS
// RUN: sea-vc %s --program=sat --lower --widen-memory=8 --mem=lambda 2>&1 | FileCheck %s --check-prefix=SAT

// SIZES: vc.widen-memory: bytes=1 classes=4 k=8 other=0 widened-2=1 widened-4=1 widened-8=1
// FIXED: vc.widen-memory: bytes=3 classes=4 k=8 other=0 widened-2=0 widened-4=0 widened-8=1
// MAX4: vc.widen-memory: bytes=1 classes=4 k=4 other=0 widened-2=1 widened-4=2 widened-8=0
// WORDS: words.check: ok
// UNSAT: result: unsat
// SAT: result: sat
// SAT: validate: {{.*}} mismatches=0
// ORIG: replay.original: status=assert-failed {{.*}}match=yes
// CHECKED: seam-check: {{.*}} invalid=0

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @unsat {
  %a0 = sea.mem.nondet "a" [0] : !m
  %b0 = sea.mem.nondet "b" [1] : !m
  %c0 = sea.mem.nondet "c" [2] : !m
  %d0 = sea.mem.nondet "d" [3] : !m
  %n = sea.const 64 : i64
  %p = sea.alloca %n align 8 [4] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [5] : i64
  %j = sea.nondet "j" [6] : i64
  %four = sea.const 4 : i64
  %ilt = sea.icmp ult, %i, %four : i64
  sea.assume %ilt
  %jlt = sea.icmp ult, %j, %four : i64
  sea.assume %jlt
  %qi = sea.ptr.index %p[%i] strides [8] offset 0 : !sea.ptr<64>, i64
  %qj = sea.ptr.index %p[%j] strides [8] offset 0 : !sea.ptr<64>, i64
  %x64 = sea.nondet "x64" [7] : i64
  %x32 = sea.trunc %x64 : i64 to i32
  %x16 = sea.trunc %x64 : i64 to i16
  %x8 = sea.trunc %x64 : i64 to i8
  %a1 = sea.store %a0[%qi], %x64 inplace : !m, i64
  %b1 = sea.store %b0[%qi], %x32 inplace : !m, i32
  %c1 = sea.store %c0[%qi], %x16 inplace : !m, i16
  %d1 = sea.store %d0[%qi], %x8 inplace : !m, i8
  %ya = sea.load %a1[%qj] : !m -> i64
  %yb = sea.load %b1[%qj] : !m -> i32
  %yc = sea.load %c1[%qj] : !m -> i16
  %yd = sea.load %d1[%qj] : !m -> i8
  %ea = sea.icmp eq, %ya, %x64 : i64
  %eb = sea.icmp eq, %yb, %x32 : i32
  %ec = sea.icmp eq, %yc, %x16 : i16
  %ed = sea.icmp eq, %yd, %x8 : i8
  %e1 = sea.and %ea, %eb : i1
  %e2 = sea.and %ec, %ed : i1
  %e = sea.and %e1, %e2 : i1
  %same = sea.icmp eq, %i, %j : i64
  %t = sea.const true
  %ok = sea.select %same, %e, %t : i1
  sea.assert %ok
  sea.halt
}

sea.program @sat {
  %a0 = sea.mem.nondet "a" [0] : !m
  %b0 = sea.mem.nondet "b" [1] : !m
  %c0 = sea.mem.nondet "c" [2] : !m
  %d0 = sea.mem.nondet "d" [3] : !m
  %n = sea.const 64 : i64
  %p = sea.alloca %n align 8 [4] : i64 -> !sea.ptr<64>
  %i = sea.nondet "i" [5] : i64
  %j = sea.nondet "j" [6] : i64
  %four = sea.const 4 : i64
  %ilt = sea.icmp ult, %i, %four : i64
  sea.assume %ilt
  %jlt = sea.icmp ult, %j, %four : i64
  sea.assume %jlt
  %qi = sea.ptr.index %p[%i] strides [8] offset 0 : !sea.ptr<64>, i64
  %qj = sea.ptr.index %p[%j] strides [8] offset 0 : !sea.ptr<64>, i64
  %x64 = sea.nondet "x64" [7] : i64
  %x32 = sea.trunc %x64 : i64 to i32
  %x16 = sea.trunc %x64 : i64 to i16
  %x8 = sea.trunc %x64 : i64 to i8
  %a1 = sea.store %a0[%qi], %x64 inplace : !m, i64
  %b1 = sea.store %b0[%qi], %x32 inplace : !m, i32
  %c1 = sea.store %c0[%qi], %x16 inplace : !m, i16
  %d1 = sea.store %d0[%qi], %x8 inplace : !m, i8
  %ya = sea.load %a1[%qj] : !m -> i64
  %yb = sea.load %b1[%qj] : !m -> i32
  %yc = sea.load %c1[%qj] : !m -> i16
  %yd = sea.load %d1[%qj] : !m -> i8
  %ea = sea.icmp eq, %ya, %x64 : i64
  %eb = sea.icmp eq, %yb, %x32 : i32
  %ec = sea.icmp eq, %yc, %x16 : i16
  %ed = sea.icmp eq, %yd, %x8 : i8
  %e1 = sea.and %ea, %eb : i1
  %e2 = sea.and %ec, %ed : i1
  %e = sea.and %e1, %e2 : i1
  %same = sea.icmp eq, %i, %j : i64
  %t = sea.const true
  %ok = sea.and %e, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
