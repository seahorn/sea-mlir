// n-ary bvmul / bvxor (the AC normalization builds them): every solver
// encoding must keep all operands. z3 once encoded only the first two: the
// queries below said unsat (wrong), and the replay validation of a loaded
// n-ary value reported a mismatch. All four programs are sat.
// RUN: sea-vc %s --lower --seam --program=mul3 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --simplify --program=mul3 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --seam --program=xor3 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --simplify --program=xor3 | FileCheck %s --check-prefix=SAT
// RUN: %if yices %{ sea-vc %s --lower --seam --solver=yices --program=mul3 | FileCheck %s --check-prefix=SAT %}
// RUN: %if yices %{ sea-vc %s --lower --seam --solver=yices --program=xor3 | FileCheck %s --check-prefix=SAT %}
// RUN: sea-vc %s --lower --seam --program=mul3r | FileCheck %s --check-prefix=REPLAY
// RUN: sea-vc %s --lower --simplify --program=mul3r | FileCheck %s --check-prefix=REPLAY
// RUN: sea-vc %s --lower --seam --program=mul3m | FileCheck %s --check-prefix=REPLAY
// RUN: %if yices %{ sea-vc %s --lower --seam --program=mul3m --solver=yices | FileCheck %s --check-prefix=REPLAY %}
// --simplify next to --seam would be ignored: refused
// RUN: not sea-vc %s --lower --seam --simplify --program=mul3 2>&1 | FileCheck %s --check-prefix=REJECT
// RUN: not sea-vc %s --lower --seam --simplify=0 --program=mul3 2>&1 | FileCheck %s --check-prefix=REJECT
// REJECT: --simplify is the normalization without SeaM
// SAT: result: sat
// SAT: replay: assert-failed
// REPLAY: result: sat
// REPLAY: validate: checked={{[0-9]+}} mismatches=0

// a*b*c == a*b is false for a=b=1, c=2.
sea.program @mul3 {
  %a = sea.nondet "a" [0] : i8
  %b = sea.nondet "b" [1] : i8
  %c = sea.nondet "c" [2] : i8
  %ab = sea.mul %a, %b : i8
  %abc = sea.mul %ab, %c : i8
  %ok = sea.icmp eq, %abc, %ab : i8
  sea.assert %ok
  sea.halt
}

// a^b^c == a^b is false for c=1.
sea.program @xor3 {
  %a = sea.nondet "a" [0] : i8
  %b = sea.nondet "b" [1] : i8
  %c = sea.nondet "c" [2] : i8
  %ab = sea.xor %a, %b : i8
  %abc = sea.xor %ab, %c : i8
  %ok = sea.icmp eq, %abc, %ab : i8
  sea.assert %ok
  sea.halt
}

!m = !sea.mem<!sea.ptr<64>, i8>
// the loaded value is rewritten to bvmul(a, b, c); the assertion fails for c != 1
sea.program @mul3r {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c8 = sea.const 8 : i64
  %o1 = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %a = sea.nondet "a" [2] : i8
  %b = sea.nondet "b" [3] : i8
  %c = sea.nondet "c" [4] : i8
  %one = sea.const 1 : i8
  %ab = sea.mul %a, %b : i8
  %abc = sea.mul %ab, %c : i8
  %m1 = sea.store %m0[%o1], %abc inplace : !m, i8
  %l = sea.load %m1[%o1] : !m -> i8
  %pa = sea.icmp eq, %a, %one : i8
  sea.assume %pa
  %pb = sea.icmp eq, %b, %one : i8
  sea.assume %pb
  %ok = sea.icmp eq, %l, %one : i8
  sea.assert %ok
  sea.halt
}

// the load is not used by the assertion: its model value must still replay
sea.program @mul3m {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c8 = sea.const 8 : i64
  %o1 = sea.alloca %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %a = sea.nondet "a" [2] : i8
  %b = sea.nondet "b" [3] : i8
  %c = sea.nondet "c" [4] : i8
  %one = sea.const 1 : i8
  %three = sea.const 3 : i8
  %ab = sea.mul %a, %b : i8
  %abc = sea.mul %ab, %c : i8
  %m1 = sea.store %m0[%o1], %abc inplace : !m, i8
  %l = sea.load %m1[%o1] : !m -> i8
  %pa = sea.icmp eq, %a, %one : i8
  sea.assume %pa
  %pb = sea.icmp eq, %b, %one : i8
  sea.assume %pb
  %pc = sea.icmp eq, %c, %three : i8
  sea.assume %pc
  %ok = sea.icmp ne, %c, %three : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
