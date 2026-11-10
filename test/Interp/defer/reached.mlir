// A deferred violation on a run that reaches the end, or a failing assert,
// is a finding: status memory-error at the violation (the first one), with
// the op that made it reachable; exit 4. A later assert failure is not
// reported as its own: its condition may depend on the substitute values.
// RUN: not sea-run %s --program=end --input=0=9 --defer-memory-errors | FileCheck %s --check-prefix=END
// RUN: bash -c 'sea-run %s --program=end --input=0=9 --defer-memory-errors > /dev/null; test $? -eq 4'
// RUN: sea-run %s --program=end --input=0=1 --missing=zero --defer-memory-errors | FileCheck %s --check-prefix=SAFE
// RUN: not sea-run %s --program=assert --input=0=9 --defer-memory-errors | FileCheck %s --check-prefix=ASSERT
// RUN: not sea-run %s --program=assert --input=0=9 --defer-memory-errors --format=jsonl | FileCheck %s --check-prefix=JSON

// END: status: memory-error
// END-NEXT: at: loc({{.*}}reached.mlir":[[@LINE+19]]
// END-NEXT: reached: halted at={{.*}}reached.mlir:[[@LINE+20]]
// END-NEXT: message: memory access out of bounds: offset 9 in stack.2 of size 4
// END-NEXT: deferred: 2 kind=out-of-bounds at={{.*}}reached.mlir:[[@LINE+16]]
// SAFE: status: halted
// SAFE-NOT: deferred:
// ASSERT: status: memory-error
// ASSERT-NEXT: at: loc({{.*}}reached.mlir":[[@LINE+21]]
// ASSERT-NEXT: reached: assert-failed at={{.*}}reached.mlir:[[@LINE+23]]
// ASSERT-NEXT: message: memory access through the null pointer
// ASSERT-NEXT: deferred: 1 kind=null
// JSON: "status":"memory-error",{{.*}}"reached":"assert-failed","reached_at":"{{.*}}reached.mlir:[[@LINE+20]]:3"
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @end {
  %m0 = sea.mem.nondet "m" [1] : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [2] : i64 -> !sea.ptr<64>
  %i = sea.nondet "nondet_u64" [0] : i64
  %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
  %v = sea.load %m0[%q] : !pm -> i8
  %m1 = sea.store %m0[%q], %v inplace : !pm, i8
  sea.halt
}
sea.program @assert {
  %m0 = sea.mem.nondet "m" [1] : !pm
  %i = sea.nondet "nondet_u64" [0] : i64
  %z = sea.const 0 : i64
  %null = sea.ptr.null : !sea.ptr<64>
  %v = sea.load %m0[%null] : !pm -> i64
  // the substitute is 0: this assert fails
  %ok = sea.icmp ne, %v, %z : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
