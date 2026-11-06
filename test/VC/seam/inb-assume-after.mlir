// An assumption after the violating access. The store through p = a+n may
// land in b (n <= 16, assumed first: a + 16 = b); the program reads b, assumes
// the read is 0 (false exactly when the store hit b) and asserts p != b.
// Plain: the executions with p == b are blocked by the assume, the others
// satisfy the assertion: unsat. Under A-INB the store is skipped, the read is
// 0, the assume holds for p == b too, and the assertion fails: --seam answers
// sat (exit 2: the replay disagrees). The answer is conditional on A-INB (not discharged): this program
// violates it, so the sat is not a counterexample of the program and it does
// not replay (assume-blocked). The test pins that: plain / --seam-inb=0 answer unsat,
// --seam sat with `seam.inb: assumed` and a failed replay, which sea-vc
// explains: the abstract program, run with the model's inputs, leaves an
// object (exit 4, vc.seam-inb-spurious), not a VC bug (exit 2).
// RUN: bash -c 'sea-vc %s --lower --solver=z3 --seam > /dev/null 2>&1; test $? -eq 4'
// RUN: sea-vc %s --lower --solver=z3 | FileCheck %s --check-prefix=PLAIN
// RUN: %if yices %{ sea-vc %s --lower --solver=yices | FileCheck %s --check-prefix=PLAIN %}
// RUN: sea-vc %s --lower --solver=z3 --seam --seam-inb=0 | FileCheck %s --check-prefix=NOINB
// RUN: not sea-vc %s --lower --solver=z3 --seam 2>&1 | FileCheck %s --check-prefix=SEAM
// RUN: %if yices %{ not sea-vc %s --lower --solver=yices --seam 2>&1 | FileCheck %s --check-prefix=SEAM %}
// PLAIN: result: unsat
// NOINB: seam.inb: off
// NOINB: result: unsat
// SEAM: seam.inb: assumed accesses={{[0-9]+}} conditions={{[1-9][0-9]*}}
// SEAM: result: sat
// SEAM: replay: assume-blocked
// SEAM: seam.inb-violation: memory access out of bounds: offset 16 in heap.1 of size 16
// SEAM: [vc.seam-inb-spurious]
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @after {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [3] : i64
  %v7 = sea.const 7 : i8
  %c16n = sea.const 16 : i64
  %small = sea.icmp ule, %n, %c16n : i64
  sea.assume %small
  %m1 = sea.store %m0[%b], %z inplace : !m, i8
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %m2 = sea.store %m1[%p], %v7 inplace : !m, i8
  %x = sea.load %m2[%b] : !m -> i8
  %zero = sea.icmp eq, %x, %z : i8
  sea.assume %zero
  %same = sea.ptr.cmp eq, %p, %b : !sea.ptr<64>
  %t = sea.const 1 : i1
  %ok = sea.xor %same, %t : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
