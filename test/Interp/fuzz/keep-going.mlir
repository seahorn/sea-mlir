// --keep-going counts failures past the first; --fuzz-time ends a run by
// the clock; the fuzz.run log has a start, new-coverage events (level 2)
// and an end.
// RUN: sea-run %s --fuzz=2000 --seed=1 --keep-going | FileCheck %s
// RUN: sea-run %s --fuzz-time=0.2 --seed=1 --keep-going | FileCheck %s --check-prefix=TIME
// RUN: sea-run %s --fuzz=200 --seed=1 --log=fuzz.run:2 2>&1 | FileCheck %s --check-prefix=LOG
// RUN: sea-run %s --fuzz=200 --seed=1 --log=fuzz.run 2>&1 | FileCheck %s --check-prefix=LOG1

// One failing assert, many failing runs; the budget is used up.
// CHECK: status: assert-failed
// CHECK: fuzz.failure: {{[1-9][0-9]+}} trial=
// CHECK: fuzz.trials: 2000 seeds=0
// TIME: fuzz.trials: {{[0-9]+}} seeds=0 time=
// TIME: fuzz.status:
// LOG: log: fuzz.run start program=@main
// LOG: log: fuzz.run new-coverage trial=1
// LOG: log: fuzz.run end trials=
// LOG1: log: fuzz.run start
// LOG1-NOT: new-coverage
// LOG1: log: fuzz.run end
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c100 = sea.const 100 : i8
  %ok = sea.icmp ult, %x, %c100 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
