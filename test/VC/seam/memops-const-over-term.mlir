// A read of a constant memory whose value holds a memory-operation term:
// `sea.mem.const %x` with %x loaded through a memset of symbolic length.
// The boundary translation had no CONST_ARRAY case: when the rewriting did
// not remove the read (--seam-row-expand=0, --seam-rule-limit), the read
// fell through to the refusal (vc.memop-term-at-boundary, exit 1).
// The fix treats every cell as the value: @eq (y = x) unsat in every mode, @ne sat and replayed.
// RUN: sea-vc %s --program=eq --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=eq --lower --seam | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=eq --lower --seam --seam-row-expand=0 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=eq --lower --seam --seam-rule-limit=3 2>&1 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=eq --lower --seam --seam-row-expand=0 --seam-inb=0 | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=eq --lower --seam --seam-row-expand=0 --seam-check | FileCheck %s --check-prefix=CHECK-VALID
// RUN: sea-vc %s --program=ne --lower --seam --seam-row-expand=0 | FileCheck %s --check-prefix=SAT
// UNSAT: result: unsat
// CHECK-VALID: result: unsat
// CHECK-VALID: seam-check: {{.*}} invalid=0
// SAT: result: sat
// SAT: replay: assert-failed
// SAT: validate: checked={{[0-9]+}} mismatches=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @eq {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [3] : i64
  %v = sea.const 99 : i8
  %m1 = sea.memset %m0[%A], %v, %n inplace : !m
  %c2 = sea.const 2 : i64
  %p = sea.ptr.add %A, %c2 : !sea.ptr<64>, i64
  %x = sea.load %m1[%p] : !m -> i8
  %k = sea.mem.const %x : !m
  %y = sea.load %k[%B] : !m -> i8
  %e = sea.icmp eq, %y, %x : i8
  sea.assert %e
  sea.halt
}
sea.program @ne {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c8 = sea.const 8 : i64
  %A = sea.malloc %c8 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [3] : i64
  %v = sea.const 99 : i8
  %m1 = sea.memset %m0[%A], %v, %n inplace : !m
  %c2 = sea.const 2 : i64
  %p = sea.ptr.add %A, %c2 : !sea.ptr<64>, i64
  %x = sea.load %m1[%p] : !m -> i8
  %k = sea.mem.const %x : !m
  %y = sea.load %k[%B] : !m -> i8
  %e = sea.icmp ne, %y, %x : i8
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
