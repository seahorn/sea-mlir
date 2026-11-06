// The out-of-bounds explanation of a failed --seam replay needs the model's
// own execution. The model's initial cell is 1, so
// every access is in bounds; the replay fails only because of the documented
// load of an earlier memory version (exit 2). Rerunning the abstract program
// with zero memory would invent offset 255 and blame A-INB; sea-vc instead
// refuses to guess: the run needs a nondet memory cell, so `undetermined`
// and exit 2 stay.
// RUN: not sea-vc %s --lower --lower-opt=false --solver=z3 --seam 2>&1 | FileCheck %s
// RUN: %if yices %{ not sea-vc %s --lower --lower-opt=false --solver=yices --seam 2>&1 | FileCheck %s %}
// RUN: bash -c 'sea-vc %s --lower --lower-opt=false --solver=z3 --seam > /dev/null 2>&1; test $? -eq 2'
// CHECK: result: sat
// CHECK: replay: no-clobber-violated: {{.*}}inb-witness-memory.mlir:30:10
// CHECK: seam.inb-violation: undetermined
// CHECK-NOT: vc.seam-inb-spurious
// CHECK: [vc.replay-mismatch]
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @zero_memory_diagnostic {
  %m0 = sea.mem.nondet "r" [0] : !m
  %sz = sea.const 16 : i64
  %a = sea.malloc %sz align 16 [1] : i64 -> !sea.ptr<64>
  %x = sea.load %m0[%a] : !m -> i8
  %one = sea.const 1 : i8
  %off8 = sea.sub %x, %one : i8
  %off = sea.zext %off8 : i8 to i64
  %p = sea.ptr.add %a, %off : !sea.ptr<64>, i64
  %seven = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %seven inplace : !m, i8
  %xone = sea.icmp eq, %x, %one : i8
  sea.assume %xone
  %old = sea.load %m0[%a] : !m -> i8
  %ok = sea.icmp ne, %old, %one : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
