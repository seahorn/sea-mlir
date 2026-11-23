// --check-inb --symbolic-size=assert: the bound of an allocation of
// symbolic size is a premise assert, a part of its own. A run that fails
// it is the job's answer, in the premise class: its premise.failed line
// (allocation, size, room) and exit 4, not a bounds violation; the job's
// `result:` is unknown (a verdict only with exit 0). Nothing is discharged.
// RUN: bash -c 'sea-vc %s --check-inb --lower --seam --ackermannize=off --symbolic-size=assert > %t.out 2> %t.err; test $? -eq 4'
// RUN: FileCheck %s < %t.out
// CHECK: check-inb.part: {{[0-9]+}} id=-1 at=a.c:3:7 result=premise-failed status=4
// CHECK-NEXT: premise.failed: symbolic-size-bound size={{[0-9]+}} room=4096 at=a.c:3:7 part={{[0-9]+}}
// CHECK: check-inb.parts: {{.*}} other=0 skipped=0 premise=1
// CHECK: check-inb.discharges: none under=
// CHECK: {{^}}result: unknown
// CHECK-NOT: result:
// RUN: not sea-vc %s --check-inb --lower --seam --ackermannize=off --symbolic-size=assert --format=jsonl 2>/dev/null | FileCheck %s --check-prefix=JSONL
// JSONL: {"kind":"premise","status":"failed","name":"symbolic-size-bound",{{.*}}"room":4096,"at":"a.c:3:7",{{.*}}"part":{{[0-9]+}},
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @premise {
  %m0 = sea.mem.nondet "m" [0] : !m
  %k = sea.nondet "k" [1] : i64
  %c1 = sea.const 1 : i64
  %ge = sea.icmp uge, %k, %c1 : i64
  sea.assume %ge
  %a = sea.malloc %k align 8 [2] : i64 -> !sea.ptr<64> loc("a.c":3:7)
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%a], %v inplace : !m, i8 loc("a.c":4:3)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
