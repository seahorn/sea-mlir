// R-LEAVES: a read at an ite address whose every leaf read is decided down
// the whole memory is split into the reads at its leaves (row.leaf-split);
// a split pointer load whose bytes then share one ite spine is the ite of
// the stored pointers (ptr.load-resolve-leaves). The shape: four field
// pointers of one object S stored unconditionally, then a 4-way switch
// passes the address of one field into a block argument and one pointer
// load reads through it. Without the split, the read keeps the whole ite
// address below the stores (a remainder read of the initial memory under
// guards that together cover every leaf), the loaded pointer is
// from_bits(concat(byte ites)) and the store through it leaves its compares
// open. Byte mode (split loads) and width terms (--seam-wide).
// RUN: sea-vc %s --lower --program=account --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,BYTE
// RUN: sea-vc %s --lower --program=account --seam --seam-wide --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,WIDE
// RUN: sea-vc %s --lower --program=account --seam --seam-leaf-split=0 --seam-stats 2>&1 | FileCheck %s --check-prefix=OFF
// @hit: the same, the store through the loaded pointer hits P5 when k = 0:
// sat, and the counterexample replays.
// RUN: sea-vc %s --lower --program=hit --seam 2>&1 | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program=hit --seam --seam-wide 2>&1 | FileCheck %s --check-prefix=SAT
// (z3 runs out of the 10 s check limit on some row.leaf-split firings, a
// z3 performance case: Bitwuzla and Yices check them)
// RUN: %if bitwuzla %{ sea-vc %s --lower --program=account --seam --seam-check --solver=bitwuzla 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: %if yices %{ sea-vc %s --lower --program=account --seam --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// RUN: %if yices %{ sea-vc %s --lower --program=account --seam --seam-wide --seam-check --solver=yices 2>&1 | FileCheck %s --check-prefix=CHK %}
// CHECK-NOT: seam.alias.open
// CHECK-NOT: seam.ptr.from-bits-left
// BYTE: seam.rule.ptr.load-resolve-leaves: 1
// CHECK: seam.rule.row.leaf-split: {{[1-9]}}
// WIDE-NOT: seam.rule.wide.split-read
// CHECK: result: unsat
// OFF: seam.alias.open.all: {{[1-9]}}
// OFF-NOT: seam.rule.row.leaf-split
// OFF: result: unsat
// SAT: result: sat
// SAT: validate: {{.*}}mismatches=0
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @account {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c40 = sea.const 40 : i64
  %c8 = sea.const 8 : i64
  %c24 = sea.const 24 : i64
  %c32 = sea.const 32 : i64
  %S = sea.alloca %c40 align 8 [1] : i64 -> !sea.ptr<64>
  %P5 = sea.alloca %c8 align 8 [5] : i64 -> !sea.ptr<64>
  %P6 = sea.alloca %c8 align 8 [6] : i64 -> !sea.ptr<64>
  %P7 = sea.alloca %c8 align 8 [7] : i64 -> !sea.ptr<64>
  %P8 = sea.alloca %c8 align 8 [8] : i64 -> !sea.ptr<64>
  %S8 = sea.ptr.add %S, %c8 : !sea.ptr<64>, i64
  %S24 = sea.ptr.add %S, %c24 : !sea.ptr<64>, i64
  %S32 = sea.ptr.add %S, %c32 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%S], %P5 inplace : !m, !sea.ptr<64>
  %m2 = sea.store %m1[%S8], %P6 inplace : !m, !sea.ptr<64>
  %m3 = sea.store %m2[%S24], %P7 inplace : !m, !sea.ptr<64>
  %m4 = sea.store %m3[%S32], %P8 inplace : !m, !sea.ptr<64>
  %k = sea.nondet "k" [3] : i8
  %z = sea.const 0 : i8
  %one = sea.const 1 : i8
  %two = sea.const 2 : i8
  %e0 = sea.icmp eq, %k, %z : i8
  sea.cond_br %e0, ^a0, ^n1
^n1:
  %e1 = sea.icmp eq, %k, %one : i8
  sea.cond_br %e1, ^a1, ^n2
^n2:
  %e2 = sea.icmp eq, %k, %two : i8
  sea.cond_br %e2, ^a2, ^a3
^a0:
  sea.br ^j(%S : !sea.ptr<64>)
^a1:
  sea.br ^j(%S8 : !sea.ptr<64>)
^a2:
  sea.br ^j(%S32 : !sea.ptr<64>)
^a3:
  sea.br ^j(%S24 : !sea.ptr<64>)
^j(%A : !sea.ptr<64>):
  %q = sea.load %m4[%A] : !m -> !sea.ptr<64>
  %v = sea.const 77 : i64
  %m5 = sea.store %m4[%q], %v inplace : !m, i64
  %r = sea.load %m5[%S] : !m -> !sea.ptr<64>
  %ok = sea.ptr.cmp eq, %r, %P5 : !sea.ptr<64>
  sea.assert %ok
  sea.halt
}
sea.program @hit {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c40 = sea.const 40 : i64
  %c8 = sea.const 8 : i64
  %c24 = sea.const 24 : i64
  %c32 = sea.const 32 : i64
  %S = sea.alloca %c40 align 8 [1] : i64 -> !sea.ptr<64>
  %P5 = sea.alloca %c8 align 8 [5] : i64 -> !sea.ptr<64>
  %P6 = sea.alloca %c8 align 8 [6] : i64 -> !sea.ptr<64>
  %P7 = sea.alloca %c8 align 8 [7] : i64 -> !sea.ptr<64>
  %P8 = sea.alloca %c8 align 8 [8] : i64 -> !sea.ptr<64>
  %S8 = sea.ptr.add %S, %c8 : !sea.ptr<64>, i64
  %S24 = sea.ptr.add %S, %c24 : !sea.ptr<64>, i64
  %S32 = sea.ptr.add %S, %c32 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%S], %P5 inplace : !m, !sea.ptr<64>
  %m2 = sea.store %m1[%S8], %P6 inplace : !m, !sea.ptr<64>
  %m3 = sea.store %m2[%S24], %P7 inplace : !m, !sea.ptr<64>
  %m4 = sea.store %m3[%S32], %P8 inplace : !m, !sea.ptr<64>
  %k = sea.nondet "k" [3] : i8
  %z = sea.const 0 : i8
  %one = sea.const 1 : i8
  %two = sea.const 2 : i8
  %e0 = sea.icmp eq, %k, %z : i8
  sea.cond_br %e0, ^a0, ^n1
^n1:
  %e1 = sea.icmp eq, %k, %one : i8
  sea.cond_br %e1, ^a1, ^n2
^n2:
  %e2 = sea.icmp eq, %k, %two : i8
  sea.cond_br %e2, ^a2, ^a3
^a0:
  sea.br ^j(%S : !sea.ptr<64>)
^a1:
  sea.br ^j(%S8 : !sea.ptr<64>)
^a2:
  sea.br ^j(%S32 : !sea.ptr<64>)
^a3:
  sea.br ^j(%S24 : !sea.ptr<64>)
^j(%A : !sea.ptr<64>):
  %q = sea.load %m4[%A] : !m -> !sea.ptr<64>
  %v = sea.const 77 : i64
  %m5 = sea.store %m4[%q], %v inplace : !m, i64
  %r = sea.load %m5[%P5] : !m -> i64
  %ok = sea.icmp ne, %r, %v : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
