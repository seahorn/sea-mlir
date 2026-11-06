// Several asserts merged by sea-merge-asserts (sea-vc --lower): the replay
// names the original assert that failed, by its own location, in `at:` and
// `assert-op:`, also when that location is a name without a file position
// (then its position in this file, and the name). (The merged assert is in
// an exit block located at the program header.)
// RUN: sea-vc %s --program=first --lower | FileCheck %s --check-prefix=FIRST
// RUN: sea-vc %s --program=second --lower | FileCheck %s --check-prefix=SECOND
// RUN: sea-vc %s --program=second --lower --format=jsonl \
// RUN:   | FileCheck %s --check-prefix=JSON
// RUN: sea-opt %s --sea-merge-asserts --mlir-print-debuginfo \
// RUN:   | sea-run - --program=first --input=0=6 | FileCheck %s --check-prefix=RUN1

// RUN1: status: assert-failed
// RUN1-NEXT: at: loc("claim.small"("<stdin>":{{[0-9]+}}:{{[0-9]+}})){{$}}
// RUN1: assert-op: <stdin>:{{[0-9]+}}:{{[0-9]+}} name=claim.small{{$}}

// FIRST: result: sat
// FIRST: replay: assert-failed{{$}}
// FIRST: at: loc("claim.small"("{{.*}}merged-assert-location.mlir":[[#L1:@LINE+10]]:3)){{$}}
// FIRST: replay: assert-op: {{.*}}merged-assert-location.mlir:[[#L1]]:3 name=claim.small{{$}}
// FIRST: validate: checked={{[0-9]+}} mismatches=0
sea.program @first {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("t.c":3:11)
  %c5 = sea.const 5 : i8
  %c3 = sea.const 3 : i8
  %ne3 = sea.icmp ne, %x, %c3 : i8
  sea.assume %ne3
  %lt = sea.icmp ult, %x, %c5 : i8 loc("t.c":5:10)
  sea.assert %lt loc("claim.small")
  %ne = sea.icmp ne, %x, %c3 : i8 loc("t.c":7:10)
  sea.assert %ne loc("claim.ne"("t.c":7:3))
  sea.halt
}

// SECOND: result: sat
// SECOND: replay: assert-failed{{$}}
// SECOND: at: loc("claim.ne"("t.c":7:3)){{$}}
// SECOND: replay: assert-op: t.c:7:3 name=claim.ne{{$}}
// JSON: "kind":"replay"
// JSON-SAME: "loc":"loc(\"claim.ne\"(\"t.c\":7:3))"
// JSON-SAME: "op":"t.c:7:3","op_name":"claim.ne"
sea.program @second {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("t.c":3:11)
  %c5 = sea.const 5 : i8
  %c3 = sea.const 3 : i8
  %lt5 = sea.icmp ult, %x, %c5 : i8
  sea.assume %lt5
  %lt = sea.icmp ult, %x, %c5 : i8 loc("t.c":5:10)
  sea.assert %lt loc("claim.small")
  %ne = sea.icmp ne, %x, %c3 : i8 loc("t.c":7:10)
  sea.assert %ne loc("claim.ne"("t.c":7:3))
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
