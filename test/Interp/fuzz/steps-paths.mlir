// The cost and path counters of --fuzz. steps= counts the ops executed over
// all trials: a straight-line run is one plain run's steps per trial. A
// path is a trial's block sequence: two independent diamonds give 4 paths,
// whatever the trials; a path is a measure, not corpus feedback.
// RUN: sea-run %s --program=line --input=0=1 > %t.line
// RUN: sea-run %s --program=line --fuzz=100 --seed=1 >> %t.line
// RUN: FileCheck %s --check-prefix=LINE < %t.line
// RUN: sea-run %s --program=diamonds --fuzz=2000 --seed=1 --fuzz-coverage-out=%t.cov | FileCheck %s --check-prefix=PATHS
// RUN: FileCheck %s --check-prefix=COV < %t.cov
// A loop: each trip count is another path (the trip count is the nondet
// value, < 8 by the assume): the 9 paths are the 8 trip counts plus the run
// the assume blocks in the entry.
// RUN: sea-run %s --program=loop --fuzz=2000 --seed=1 | FileCheck %s --check-prefix=LOOP

// LINE: steps: [[#K:]]
// LINE: fuzz.trials: 100 {{.*}} steps=[[#mul(K,100)]] steps/s={{[0-9]+}}

// PATHS: fuzz.coverage: blocks=7 blocks-total=7 edges=9 {{.*}} paths=4{{$}}

// COV: totals: blocks-total=7 assumes-total=0 paths-capped=0
// COV-COUNT-7: block:
// COV-NOT: block:
// COV: edge: -1 0
// COV-COUNT-4: path: 0x{{[0-9a-f]+}}
// COV-NOT: path:

// LOOP: fuzz.coverage: {{.*}} paths=9{{$}}
sea.program @line {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c = sea.const 3 : i8
  %y = sea.add %x, %c : i8
  %t = sea.const 1 : i1
  sea.assert %t
  sea.halt
}

sea.program @diamonds {
  %a = sea.nondet "nondet_bool" [0] : i1
  %b = sea.nondet "nondet_bool" [1] : i1
  sea.cond_br %a, ^a1, ^a2
^a1:
  sea.br ^mid
^a2:
  sea.br ^mid
^mid:
  sea.cond_br %b, ^b1, ^b2
^b1:
  sea.br ^end
^b2:
  sea.br ^end
^end:
  sea.halt
}

sea.program @loop {
  %n = sea.nondet "nondet_u8" [0] : i8
  %c8 = sea.const 8 : i8
  %lt = sea.icmp ult, %n, %c8 : i8
  sea.assume %lt
  %zero = sea.const 0 : i8
  %one = sea.const 1 : i8
  sea.br ^head(%zero : i8)
^head(%i : i8):
  %more = sea.icmp ult, %i, %n : i8
  sea.cond_br %more, ^body, ^done
^body:
  %j = sea.add %i, %one : i8
  sea.br ^head(%j : i8)
^done:
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
