// A load that reads a stale memory version makes a reachable error
// unreachable in the VC (a vacuous unsat). The program is SeaHorn's export of
//
//   struct A { char *p; long len; };
//   struct B { char *p; long len; };            // same layout as A
//   static void set_len(struct A *a) { a->len = 1; }
//   struct B *v = malloc(2 * sizeof(struct B));
//   long i = nd(); assume(i == 1);
//   v[0].len = 0; v[1].len = 0;
//   set_len((struct A *)&v[i]);                  // store, TBAA tag A::len
//   long j = nd(); assume(j == 1);
//   assume(v[j].len == 1);                       // load,  TBAA tag B::len
//   error();
//
// at -O0. The store and the load have different TBAA tags (a strict-aliasing
// violation), and SeaHorn's shadow-memory optimizer, which trusts TBAA, gives
// the load the version %m2, from before set_len's store %m3. sea-vc is
// faithful to that program: unsat. sea-run executes memory in place, so it
// reads the stored 1, reaches the assertion, and warns that the load names an
// earlier memory version, the cheap detector for this case.
// sea-vc's unsat path does not replay, so it prints no
// interp.earlier-version-loads count (see earlier-version-count.mlir).
// RUN: sea-vc %s --lower 2>&1 | FileCheck %s
// RUN: sea-run %s --input=2=1,3=1 2>&1 | FileCheck %s --check-prefix=INTERP
// CHECK: result: unsat
// CHECK-NOT: interp.earlier-version-loads
// INTERP: Warning: load of an earlier memory version{{.*}}[interp.load-earlier-version]
// INTERP: status: assert-failed
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @"main" {
^bb0:
  %m0 = sea.mem.nondet "heap" [0] : !m
  %sz = sea.const 32 : i64
  %v = sea.malloc %sz align 16 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "nd_i64" [2] : i64
  %one = sea.const 1 : i64
  %zero = sea.const 0 : i64
  %ci = sea.icmp eq, %i, %one : i64
  sea.assume %ci
  %p0 = sea.ptr.index %v [] strides [] offset 8 inbounds : !sea.ptr<64>
  %m1 = sea.store %m0[%p0], %zero inplace align 8 : !m, i64
  %p1 = sea.ptr.index %v [] strides [] offset 24 inbounds : !sea.ptr<64>
  %m2 = sea.store %m1[%p1], %zero inplace align 8 : !m, i64
  // set_len((struct A *)&v[i]): a->len = 1
  %vi = sea.ptr.index %v [%i] strides [16] offset 0 inbounds : !sea.ptr<64>, i64
  %pa = sea.ptr.index %vi [] strides [] offset 8 inbounds : !sea.ptr<64>
  %m3 = sea.store %m2[%pa], %one inplace align 8 : !m, i64
  %j = sea.nondet "nd_i64" [3] : i64
  %cj = sea.icmp eq, %j, %one : i64
  sea.assume %cj
  // v[j].len: the stale version %m2, not %m3
  %pb = sea.ptr.index %v [%j] strides [16] offset 8 inbounds : !sea.ptr<64>, i64
  %len = sea.load %m2[%pb] align 8 : !m -> i64
  %cl = sea.icmp eq, %len, %one : i64
  sea.assume %cl
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
