// With --defer-memory-errors, a violation on a run that reaches the end is
// a memory-safety finding: a failure file, replayed and printed as a plain
// run, a fuzz.memory-safety line, exit 4; the fuzzer stops at it. Its file
// replays through --input-file to the same finding.
// RUN: rm -rf %t && mkdir -p %t
// RUN: bash -c 'sea-run %s --program=reach --fuzz=500 --seed=3 --defer-memory-errors --fuzz-out=%t/out > %t/fuzz.txt; test $? -eq 4'
// RUN: FileCheck %s --check-prefix=FUZZ < %t/fuzz.txt
// RUN: cat %t/out/failure-*.txt > %t/failure.txt
// RUN: FileCheck %s --check-prefix=HEADER < %t/failure.txt
// RUN: not sea-run %s --program=reach --input-file=%t/failure.txt --defer-memory-errors | FileCheck %s --check-prefix=REPLAY
// Runs whose violation a later assume blocks are not findings.
// RUN: sea-run %s --program=late --fuzz=500 --seed=3 --defer-memory-errors | FileCheck %s --check-prefix=LATE

// FUZZ: status: memory-error
// FUZZ-NEXT: at: loc({{.*}}memory-safety.mlir":[[@LINE+22]]
// FUZZ-NEXT: reached: halted at={{.*}}memory-safety.mlir:[[@LINE+22]]
// FUZZ-NEXT: message: memory access out of bounds
// FUZZ: fuzz.memory-safety: 1 trial={{[0-9]+}} file={{.*}}failure-{{[0-9]+}}.txt op=sea.load at={{.*}}memory-safety.mlir:[[@LINE+19]]
// FUZZ-NEXT: fuzz.memory-safety-message: memory access out of bounds
// FUZZ: fuzz.status: halted={{[0-9]+}} assert-failed=0 assume-blocked=0 step-limit=0 error=0 memory-error=1
// FUZZ-NEXT: fuzz.deferred: 1 memory-error=1 assume-blocked=0 step-limit=0 error=0
// HEADER: # sea-run --fuzz: trial={{[0-9]+}} status=memory-error
// HEADER-NEXT: # replay with: sea-run --defer-memory-errors --input-file=
// REPLAY: status: memory-error
// REPLAY-NEXT: at: loc({{.*}}memory-safety.mlir":[[@LINE+12]]
// LATE-NOT: fuzz.memory-safety
// LATE-NOT: fuzz.memory-check
// LATE: fuzz.status: halted={{[1-9][0-9]*}} assert-failed=0 assume-blocked={{[1-9][0-9]*}} step-limit=0 error=0 memory-error=0
// LATE-NEXT: fuzz.deferred: [[N:[1-9][0-9]*]] memory-error=0 assume-blocked=[[N]] step-limit=0 error=0
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @reach {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "nondet_u64" [2] : i64
  %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
  %v = sea.load %m0[%q] : !pm -> i8
  sea.halt
}
sea.program @late {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "nondet_u64" [2] : i64
  %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
  %v = sea.load %m0[%q] : !pm -> i8
  %ok = sea.icmp ult, %i, %n : i64
  sea.assume %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
