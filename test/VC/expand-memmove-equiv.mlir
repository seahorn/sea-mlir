// RUN: sea-vc %s --program=equiv --lower --seam --expand-memmove=4 | FileCheck %s
// RUN: sed 's/sea.const 4 : i64 loc("len")/sea.const 3 : i64 loc("len")/' %s | sea-vc - --program=equiv --lower --seam --expand-memmove=4 | FileCheck %s --check-prefix=MUT
// RUN: sea-vc %s --program=interleaved --lower --seam | FileCheck %s --check-prefix=INTER

// sea-expand-small-memmove is memmove, overlap included, proved by z3.
// Object q starts as a copy of object p (a 16-byte memmove: over K, kept);
// then p gets a memmove of a symbolic length n = 4 (kept: SeaM's exact
// mcopy term) and q one of the constant length 4 (expanded), both between
// the same symbolic, possibly overlapping offsets; every cell of p and q
// agrees. The mutant: the expanded one copies 3 bytes, a cell differs.
// CHECK: vc.expand-memmove: k=4 expanded=1 bytes=4 kept-symbolic=1 kept-over-k=1
// CHECK: result: unsat
// MUT: result: sat
// MUT: replay: assert-failed
// @interleaved: the same check with q's copy written by hand as a chain
// that loads each byte from the current version right before storing it
// (no memmove): sat, since an overlapping copy then reads bytes it already
// wrote. This pins that the check sees the order of loads and stores.
// INTER: result: sat
// INTER: replay: assert-failed
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @equiv {
  %m0 = sea.mem.nondet "m" [1] : !m
  %c16 = sea.const 16 : i64
  %p = sea.alloca %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %q = sea.alloca %c16 align 8 [6] : i64 -> !sea.ptr<64>
  %mc = sea.memmove %m0[%q], %m0[%p], %c16 inplace : !m
  %a = sea.nondet "a" [3] : i64
  %b = sea.nondet "b" [4] : i64
  %n = sea.nondet "n" [5] : i64
  %c12 = sea.const 12 : i64
  %four = sea.const 4 : i64 loc("len")
  %c4 = sea.const 4 : i64
  %fa = sea.icmp ule, %a, %c12 : i64
  %fb = sea.icmp ule, %b, %c12 : i64
  %fn = sea.icmp eq, %n, %c4 : i64
  %f0 = sea.and %fa, %fb : i1
  %f1 = sea.and %f0, %fn : i1
  sea.assume %f1
  %d = sea.ptr.add %p, %a : !sea.ptr<64>, i64
  %s = sea.ptr.add %p, %b : !sea.ptr<64>, i64
  %e = sea.ptr.add %q, %a : !sea.ptr<64>, i64
  %t = sea.ptr.add %q, %b : !sea.ptr<64>, i64
  %m1 = sea.memmove %mc[%d], %mc[%s], %n inplace : !m
  %m2 = sea.memmove %m1[%e], %m1[%t], %four inplace : !m
  %o0 = sea.const 0 : i64
  %p0 = sea.ptr.add %p, %o0 : !sea.ptr<64>, i64
  %q0 = sea.ptr.add %q, %o0 : !sea.ptr<64>, i64
  %x0 = sea.load %m2[%p0] : !m -> i8
  %y0 = sea.load %m2[%q0] : !m -> i8
  %k0 = sea.icmp eq, %x0, %y0 : i8
  %o1 = sea.const 1 : i64
  %p1 = sea.ptr.add %p, %o1 : !sea.ptr<64>, i64
  %q1 = sea.ptr.add %q, %o1 : !sea.ptr<64>, i64
  %x1 = sea.load %m2[%p1] : !m -> i8
  %y1 = sea.load %m2[%q1] : !m -> i8
  %k1 = sea.icmp eq, %x1, %y1 : i8
  %o2 = sea.const 2 : i64
  %p2 = sea.ptr.add %p, %o2 : !sea.ptr<64>, i64
  %q2 = sea.ptr.add %q, %o2 : !sea.ptr<64>, i64
  %x2 = sea.load %m2[%p2] : !m -> i8
  %y2 = sea.load %m2[%q2] : !m -> i8
  %k2 = sea.icmp eq, %x2, %y2 : i8
  %o3 = sea.const 3 : i64
  %p3 = sea.ptr.add %p, %o3 : !sea.ptr<64>, i64
  %q3 = sea.ptr.add %q, %o3 : !sea.ptr<64>, i64
  %x3 = sea.load %m2[%p3] : !m -> i8
  %y3 = sea.load %m2[%q3] : !m -> i8
  %k3 = sea.icmp eq, %x3, %y3 : i8
  %o4 = sea.const 4 : i64
  %p4 = sea.ptr.add %p, %o4 : !sea.ptr<64>, i64
  %q4 = sea.ptr.add %q, %o4 : !sea.ptr<64>, i64
  %x4 = sea.load %m2[%p4] : !m -> i8
  %y4 = sea.load %m2[%q4] : !m -> i8
  %k4 = sea.icmp eq, %x4, %y4 : i8
  %o5 = sea.const 5 : i64
  %p5 = sea.ptr.add %p, %o5 : !sea.ptr<64>, i64
  %q5 = sea.ptr.add %q, %o5 : !sea.ptr<64>, i64
  %x5 = sea.load %m2[%p5] : !m -> i8
  %y5 = sea.load %m2[%q5] : !m -> i8
  %k5 = sea.icmp eq, %x5, %y5 : i8
  %o6 = sea.const 6 : i64
  %p6 = sea.ptr.add %p, %o6 : !sea.ptr<64>, i64
  %q6 = sea.ptr.add %q, %o6 : !sea.ptr<64>, i64
  %x6 = sea.load %m2[%p6] : !m -> i8
  %y6 = sea.load %m2[%q6] : !m -> i8
  %k6 = sea.icmp eq, %x6, %y6 : i8
  %o7 = sea.const 7 : i64
  %p7 = sea.ptr.add %p, %o7 : !sea.ptr<64>, i64
  %q7 = sea.ptr.add %q, %o7 : !sea.ptr<64>, i64
  %x7 = sea.load %m2[%p7] : !m -> i8
  %y7 = sea.load %m2[%q7] : !m -> i8
  %k7 = sea.icmp eq, %x7, %y7 : i8
  %o8 = sea.const 8 : i64
  %p8 = sea.ptr.add %p, %o8 : !sea.ptr<64>, i64
  %q8 = sea.ptr.add %q, %o8 : !sea.ptr<64>, i64
  %x8 = sea.load %m2[%p8] : !m -> i8
  %y8 = sea.load %m2[%q8] : !m -> i8
  %k8 = sea.icmp eq, %x8, %y8 : i8
  %o9 = sea.const 9 : i64
  %p9 = sea.ptr.add %p, %o9 : !sea.ptr<64>, i64
  %q9 = sea.ptr.add %q, %o9 : !sea.ptr<64>, i64
  %x9 = sea.load %m2[%p9] : !m -> i8
  %y9 = sea.load %m2[%q9] : !m -> i8
  %k9 = sea.icmp eq, %x9, %y9 : i8
  %o10 = sea.const 10 : i64
  %p10 = sea.ptr.add %p, %o10 : !sea.ptr<64>, i64
  %q10 = sea.ptr.add %q, %o10 : !sea.ptr<64>, i64
  %x10 = sea.load %m2[%p10] : !m -> i8
  %y10 = sea.load %m2[%q10] : !m -> i8
  %k10 = sea.icmp eq, %x10, %y10 : i8
  %o11 = sea.const 11 : i64
  %p11 = sea.ptr.add %p, %o11 : !sea.ptr<64>, i64
  %q11 = sea.ptr.add %q, %o11 : !sea.ptr<64>, i64
  %x11 = sea.load %m2[%p11] : !m -> i8
  %y11 = sea.load %m2[%q11] : !m -> i8
  %k11 = sea.icmp eq, %x11, %y11 : i8
  %o12 = sea.const 12 : i64
  %p12 = sea.ptr.add %p, %o12 : !sea.ptr<64>, i64
  %q12 = sea.ptr.add %q, %o12 : !sea.ptr<64>, i64
  %x12 = sea.load %m2[%p12] : !m -> i8
  %y12 = sea.load %m2[%q12] : !m -> i8
  %k12 = sea.icmp eq, %x12, %y12 : i8
  %o13 = sea.const 13 : i64
  %p13 = sea.ptr.add %p, %o13 : !sea.ptr<64>, i64
  %q13 = sea.ptr.add %q, %o13 : !sea.ptr<64>, i64
  %x13 = sea.load %m2[%p13] : !m -> i8
  %y13 = sea.load %m2[%q13] : !m -> i8
  %k13 = sea.icmp eq, %x13, %y13 : i8
  %o14 = sea.const 14 : i64
  %p14 = sea.ptr.add %p, %o14 : !sea.ptr<64>, i64
  %q14 = sea.ptr.add %q, %o14 : !sea.ptr<64>, i64
  %x14 = sea.load %m2[%p14] : !m -> i8
  %y14 = sea.load %m2[%q14] : !m -> i8
  %k14 = sea.icmp eq, %x14, %y14 : i8
  %o15 = sea.const 15 : i64
  %p15 = sea.ptr.add %p, %o15 : !sea.ptr<64>, i64
  %q15 = sea.ptr.add %q, %o15 : !sea.ptr<64>, i64
  %x15 = sea.load %m2[%p15] : !m -> i8
  %y15 = sea.load %m2[%q15] : !m -> i8
  %k15 = sea.icmp eq, %x15, %y15 : i8
  %a1 = sea.and %k0, %k1 : i1
  %a2 = sea.and %a1, %k2 : i1
  %a3 = sea.and %a2, %k3 : i1
  %a4 = sea.and %a3, %k4 : i1
  %a5 = sea.and %a4, %k5 : i1
  %a6 = sea.and %a5, %k6 : i1
  %a7 = sea.and %a6, %k7 : i1
  %a8 = sea.and %a7, %k8 : i1
  %a9 = sea.and %a8, %k9 : i1
  %a10 = sea.and %a9, %k10 : i1
  %a11 = sea.and %a10, %k11 : i1
  %a12 = sea.and %a11, %k12 : i1
  %a13 = sea.and %a12, %k13 : i1
  %a14 = sea.and %a13, %k14 : i1
  %a15 = sea.and %a14, %k15 : i1
  sea.assert %a15
  sea.halt
}

sea.program @interleaved {
  %m0 = sea.mem.nondet "m" [1] : !m
  %c16 = sea.const 16 : i64
  %p = sea.alloca %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %q = sea.alloca %c16 align 8 [6] : i64 -> !sea.ptr<64>
  %mc = sea.memmove %m0[%q], %m0[%p], %c16 inplace : !m
  %a = sea.nondet "a" [3] : i64
  %b = sea.nondet "b" [4] : i64
  %n = sea.nondet "n" [5] : i64
  %c12 = sea.const 12 : i64
  %c4 = sea.const 4 : i64
  %fa = sea.icmp ule, %a, %c12 : i64
  %fb = sea.icmp ule, %b, %c12 : i64
  %fn = sea.icmp eq, %n, %c4 : i64
  %f0 = sea.and %fa, %fb : i1
  %f1 = sea.and %f0, %fn : i1
  sea.assume %f1
  %d = sea.ptr.add %p, %a : !sea.ptr<64>, i64
  %s = sea.ptr.add %p, %b : !sea.ptr<64>, i64
  %e = sea.ptr.add %q, %a : !sea.ptr<64>, i64
  %t = sea.ptr.add %q, %b : !sea.ptr<64>, i64
  %m1 = sea.memmove %mc[%d], %mc[%s], %n inplace : !m
  %ci0 = sea.const 0 : i64
  %ti0 = sea.ptr.add %t, %ci0 : !sea.ptr<64>, i64
  %ei0 = sea.ptr.add %e, %ci0 : !sea.ptr<64>, i64
  %vi0 = sea.load %m1[%ti0] : !m -> i8
  %mi0 = sea.store %m1[%ei0], %vi0 inplace : !m, i8
  %ci1 = sea.const 1 : i64
  %ti1 = sea.ptr.add %t, %ci1 : !sea.ptr<64>, i64
  %ei1 = sea.ptr.add %e, %ci1 : !sea.ptr<64>, i64
  %vi1 = sea.load %mi0[%ti1] : !m -> i8
  %mi1 = sea.store %mi0[%ei1], %vi1 inplace : !m, i8
  %ci2 = sea.const 2 : i64
  %ti2 = sea.ptr.add %t, %ci2 : !sea.ptr<64>, i64
  %ei2 = sea.ptr.add %e, %ci2 : !sea.ptr<64>, i64
  %vi2 = sea.load %mi1[%ti2] : !m -> i8
  %mi2 = sea.store %mi1[%ei2], %vi2 inplace : !m, i8
  %ci3 = sea.const 3 : i64
  %ti3 = sea.ptr.add %t, %ci3 : !sea.ptr<64>, i64
  %ei3 = sea.ptr.add %e, %ci3 : !sea.ptr<64>, i64
  %vi3 = sea.load %mi2[%ti3] : !m -> i8
  %m2 = sea.store %mi2[%ei3], %vi3 inplace : !m, i8
  %o0 = sea.const 0 : i64
  %p0 = sea.ptr.add %p, %o0 : !sea.ptr<64>, i64
  %q0 = sea.ptr.add %q, %o0 : !sea.ptr<64>, i64
  %x0 = sea.load %m2[%p0] : !m -> i8
  %y0 = sea.load %m2[%q0] : !m -> i8
  %k0 = sea.icmp eq, %x0, %y0 : i8
  %o1 = sea.const 1 : i64
  %p1 = sea.ptr.add %p, %o1 : !sea.ptr<64>, i64
  %q1 = sea.ptr.add %q, %o1 : !sea.ptr<64>, i64
  %x1 = sea.load %m2[%p1] : !m -> i8
  %y1 = sea.load %m2[%q1] : !m -> i8
  %k1 = sea.icmp eq, %x1, %y1 : i8
  %o2 = sea.const 2 : i64
  %p2 = sea.ptr.add %p, %o2 : !sea.ptr<64>, i64
  %q2 = sea.ptr.add %q, %o2 : !sea.ptr<64>, i64
  %x2 = sea.load %m2[%p2] : !m -> i8
  %y2 = sea.load %m2[%q2] : !m -> i8
  %k2 = sea.icmp eq, %x2, %y2 : i8
  %o3 = sea.const 3 : i64
  %p3 = sea.ptr.add %p, %o3 : !sea.ptr<64>, i64
  %q3 = sea.ptr.add %q, %o3 : !sea.ptr<64>, i64
  %x3 = sea.load %m2[%p3] : !m -> i8
  %y3 = sea.load %m2[%q3] : !m -> i8
  %k3 = sea.icmp eq, %x3, %y3 : i8
  %o4 = sea.const 4 : i64
  %p4 = sea.ptr.add %p, %o4 : !sea.ptr<64>, i64
  %q4 = sea.ptr.add %q, %o4 : !sea.ptr<64>, i64
  %x4 = sea.load %m2[%p4] : !m -> i8
  %y4 = sea.load %m2[%q4] : !m -> i8
  %k4 = sea.icmp eq, %x4, %y4 : i8
  %o5 = sea.const 5 : i64
  %p5 = sea.ptr.add %p, %o5 : !sea.ptr<64>, i64
  %q5 = sea.ptr.add %q, %o5 : !sea.ptr<64>, i64
  %x5 = sea.load %m2[%p5] : !m -> i8
  %y5 = sea.load %m2[%q5] : !m -> i8
  %k5 = sea.icmp eq, %x5, %y5 : i8
  %o6 = sea.const 6 : i64
  %p6 = sea.ptr.add %p, %o6 : !sea.ptr<64>, i64
  %q6 = sea.ptr.add %q, %o6 : !sea.ptr<64>, i64
  %x6 = sea.load %m2[%p6] : !m -> i8
  %y6 = sea.load %m2[%q6] : !m -> i8
  %k6 = sea.icmp eq, %x6, %y6 : i8
  %o7 = sea.const 7 : i64
  %p7 = sea.ptr.add %p, %o7 : !sea.ptr<64>, i64
  %q7 = sea.ptr.add %q, %o7 : !sea.ptr<64>, i64
  %x7 = sea.load %m2[%p7] : !m -> i8
  %y7 = sea.load %m2[%q7] : !m -> i8
  %k7 = sea.icmp eq, %x7, %y7 : i8
  %o8 = sea.const 8 : i64
  %p8 = sea.ptr.add %p, %o8 : !sea.ptr<64>, i64
  %q8 = sea.ptr.add %q, %o8 : !sea.ptr<64>, i64
  %x8 = sea.load %m2[%p8] : !m -> i8
  %y8 = sea.load %m2[%q8] : !m -> i8
  %k8 = sea.icmp eq, %x8, %y8 : i8
  %o9 = sea.const 9 : i64
  %p9 = sea.ptr.add %p, %o9 : !sea.ptr<64>, i64
  %q9 = sea.ptr.add %q, %o9 : !sea.ptr<64>, i64
  %x9 = sea.load %m2[%p9] : !m -> i8
  %y9 = sea.load %m2[%q9] : !m -> i8
  %k9 = sea.icmp eq, %x9, %y9 : i8
  %o10 = sea.const 10 : i64
  %p10 = sea.ptr.add %p, %o10 : !sea.ptr<64>, i64
  %q10 = sea.ptr.add %q, %o10 : !sea.ptr<64>, i64
  %x10 = sea.load %m2[%p10] : !m -> i8
  %y10 = sea.load %m2[%q10] : !m -> i8
  %k10 = sea.icmp eq, %x10, %y10 : i8
  %o11 = sea.const 11 : i64
  %p11 = sea.ptr.add %p, %o11 : !sea.ptr<64>, i64
  %q11 = sea.ptr.add %q, %o11 : !sea.ptr<64>, i64
  %x11 = sea.load %m2[%p11] : !m -> i8
  %y11 = sea.load %m2[%q11] : !m -> i8
  %k11 = sea.icmp eq, %x11, %y11 : i8
  %o12 = sea.const 12 : i64
  %p12 = sea.ptr.add %p, %o12 : !sea.ptr<64>, i64
  %q12 = sea.ptr.add %q, %o12 : !sea.ptr<64>, i64
  %x12 = sea.load %m2[%p12] : !m -> i8
  %y12 = sea.load %m2[%q12] : !m -> i8
  %k12 = sea.icmp eq, %x12, %y12 : i8
  %o13 = sea.const 13 : i64
  %p13 = sea.ptr.add %p, %o13 : !sea.ptr<64>, i64
  %q13 = sea.ptr.add %q, %o13 : !sea.ptr<64>, i64
  %x13 = sea.load %m2[%p13] : !m -> i8
  %y13 = sea.load %m2[%q13] : !m -> i8
  %k13 = sea.icmp eq, %x13, %y13 : i8
  %o14 = sea.const 14 : i64
  %p14 = sea.ptr.add %p, %o14 : !sea.ptr<64>, i64
  %q14 = sea.ptr.add %q, %o14 : !sea.ptr<64>, i64
  %x14 = sea.load %m2[%p14] : !m -> i8
  %y14 = sea.load %m2[%q14] : !m -> i8
  %k14 = sea.icmp eq, %x14, %y14 : i8
  %o15 = sea.const 15 : i64
  %p15 = sea.ptr.add %p, %o15 : !sea.ptr<64>, i64
  %q15 = sea.ptr.add %q, %o15 : !sea.ptr<64>, i64
  %x15 = sea.load %m2[%p15] : !m -> i8
  %y15 = sea.load %m2[%q15] : !m -> i8
  %k15 = sea.icmp eq, %x15, %y15 : i8
  %a1 = sea.and %k0, %k1 : i1
  %a2 = sea.and %a1, %k2 : i1
  %a3 = sea.and %a2, %k3 : i1
  %a4 = sea.and %a3, %k4 : i1
  %a5 = sea.and %a4, %k5 : i1
  %a6 = sea.and %a5, %k6 : i1
  %a7 = sea.and %a6, %k7 : i1
  %a8 = sea.and %a7, %k8 : i1
  %a9 = sea.and %a8, %k9 : i1
  %a10 = sea.and %a9, %k10 : i1
  %a11 = sea.and %a10, %k11 : i1
  %a12 = sea.and %a11, %k12 : i1
  %a13 = sea.and %a12, %k13 : i1
  %a14 = sea.and %a13, %k14 : i1
  %a15 = sea.and %a14, %k15 : i1
  sea.assert %a15
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
