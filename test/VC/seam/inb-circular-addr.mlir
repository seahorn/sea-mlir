// A later access whose address comes from a load that A-INB rewrites. The
// store through a+n can hit b and write 200 there; the byte read from b is
// then the offset of a store into c, a 4-byte object, and the program assumes
// that byte is 200. Plain: sat (the store into c leaves c, replayed). With
// A-INB the read of b is 0, the assumption is false: --seam answers unsat,
// `seam.inb: assumed`: conditional on A-INB, which is not discharged.
// RUN: sea-vc %s --lower --solver=z3 | FileCheck %s --check-prefix=PLAIN
// (no yices on the plain run: its model evaluation fails on the lambda of the memory)
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-inb=0 | FileCheck %s --check-prefix=NOINB
// RUN: sea-vc %s --lower --solver=z3 --seam 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: %if yices %{ sea-vc %s --lower --solver=yices --seam 2>&1 | FileCheck %s --check-prefix=SEAM %}
// PLAIN: result: sat
// PLAIN: replay: assert-failed
// NOINB: seam.inb: off
// NOINB: result: sat
// NOINB: replay: assert-failed
// NOINB: validate: checked={{[0-9]+}} mismatches=0
// SEAM-NOT: vc.seam-inb-violated
// SEAM: seam.inb: assumed accesses={{[0-9]+}} conditions={{[1-9][0-9]*}}
// SEAM: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @addr {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %c4 = sea.const 4 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %c = sea.malloc %c4 align 4 [4] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [3] : i64
  %v200 = sea.const 200 : i8
  %v1 = sea.const 1 : i8
  %m1 = sea.store %m0[%b], %z inplace : !m, i8
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %m2 = sea.store %m1[%p], %v200 inplace : !m, i8
  %x = sea.load %m2[%b] : !m -> i8
  %hit = sea.icmp eq, %x, %v200 : i8
  sea.assume %hit
  %off = sea.zext %x : i8 to i64
  %q = sea.ptr.add %c, %off : !sea.ptr<64>, i64
  %m3 = sea.store %m2[%q], %v1 inplace : !m, i8
  %y = sea.load %m3[%c] : !m -> i8
  %ok = sea.icmp eq, %y, %v1 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
