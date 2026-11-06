// RUN: not sea-vc %s --program=direct 2>&1 | FileCheck %s
// RUN: not sea-vc %s --program=direct --lower 2>&1 | FileCheck %s
// RUN: bash -c 'sea-vc %s --program=direct > /dev/null 2>&1; test $? -eq 2'
// RUN: not sea-vc %s --program=direct --format=jsonl 2>/dev/null | FileCheck %s --check-prefix=JSONL
// RUN: sea-vc %s --program=unclobbered 2>&1 | FileCheck %s --check-prefix=OK

// A load of an earlier memory version (Memory.md: allowed, on Mem-SSA's
// claim that the cell was not clobbered since) after an in-place store that
// clobbers its cell: the VC reads the version the program names, the in-place
// interpreter the current contents. Replay validation reports the mismatch
// and names the load whose claim fails (exit 2 stays: the counterexample
// does not replay). @unclobbered: the store writes another cell, the claim
// holds, the replay is clean.
// CHECK: result: sat
// CHECK: mismatch:
// CHECK: replay: no-clobber-violated: {{.*}}no-clobber-violated.mlir:29:8 (a load of an earlier memory version read a cell overwritten since; the program's Mem-SSA no-clobber claim fails here)
// CHECK: the cause is the program's Mem-SSA no-clobber claim, not the VC{{.*}}[vc.replay-mismatch]
// JSONL: {"kind":"no-clobber-violated","loc":"{{.*}}no-clobber-violated.mlir:29:8","text":"replay: no-clobber-violated:
// OK: result: sat
// OK: validate: checked={{[0-9]+}} mismatches=0
// OK-NOT: no-clobber-violated
!m = !sea.mem<!sea.addr<8>, i8>
sea.program @direct {
  %m0 = sea.mem.nondet "r" [0] : !m
  %ai = sea.nondet "a" [1] : i8
  %a = sea.addr.from_int %ai : i8 to !sea.addr<8>
  %seven = sea.const 7 : i8
  %m1 = sea.store %m0[%a], %seven inplace : !m, i8
  %x = sea.load %m0[%a] : !m -> i8
  %ok = sea.icmp eq, %x, %seven : i8
  sea.assert %ok
  sea.halt
}
sea.program @unclobbered {
  %m0 = sea.mem.nondet "r" [0] : !m
  %ai = sea.nondet "a" [1] : i8
  %a = sea.addr.from_int %ai : i8 to !sea.addr<8>
  %one = sea.const 1 : i8
  %b = sea.addr.add %a, %one : !sea.addr<8>, i8
  %seven = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %seven inplace : !m, i8
  %x = sea.load %m0[%a] : !m -> i8
  %ok = sea.icmp eq, %x, %seven : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
