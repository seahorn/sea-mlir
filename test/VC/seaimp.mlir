// RUN: not sea-vc %s --program=max 2>&1 | FileCheck %s --check-prefix=RAW
// RUN: not sea-run %s --program=max 2>&1 | FileCheck %s --check-prefix=INTERP
// RUN: sea-vc %s --lower --program=max | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program=uninit | FileCheck %s --check-prefix=SAT
// RUN: sea-opt %s --sea-lower-seaimp | sea-vc - --lower --program=uninit | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program=uninit --replay-original | FileCheck %s --check-prefix=ORIG
// RUN: sea-vc %s --lower --program=max --log=lower.pass:1 2>&1 | FileCheck %s --check-prefix=ORDER

// seaimp has meaning only through its lowering: sea-vc refuses it without
// --lower, sea-run always (docs/design/SeaImp.md).
// The seaimp pre-pass lowers the programs one at a time, in module order.
// ORDER: pass=sea-lower-seaimp on=sea.program program=@max
// ORDER-NEXT: pass=sea-lower-seaimp on=sea.program program=@uninit
// RAW: vc.seaimp
// INTERP: run.seaimp

// x = a; if (a < b) x = b; assert(x >= a): safe.
// UNSAT: result: unsat
sea.program @max {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  %a = sea.nondet "nd_int" [2] : i32
  %b = sea.nondet "nd_int" [3] : i32
  seaimp.set %x, %a : !seaimp.var<i32>
  %lt = sea.icmp slt, %a, %b : i32
  sea.cond_br %lt, ^bb1, ^bb2
^bb1:
  seaimp.set %x, %b : !seaimp.var<i32>
  sea.br ^bb2
^bb2:
  %m = seaimp.get %x : !seaimp.var<i32>
  %ok = sea.icmp sge, %m, %a : i32
  sea.assert %ok
  sea.halt
}

// Without the first set, x starts at its nondet initial value (site 1):
// the assertion fails when a >= b and x < a; the replay reads x's initial
// value from the model by its site and agrees with the VC.
// SAT: replay: assert-failed
// SAT: mismatches=0
// SAT: result: sat
// --replay-original replays the program before the rest of the lowering
// (seaimp lowered first: it has no runtime semantics).
// ORIG: replay.original: status=assert-failed {{.*}}match=yes
sea.program @uninit {
  %x = seaimp.var "x" [1] : !seaimp.var<i32>
  %a = sea.nondet "nd_int" [2] : i32
  %b = sea.nondet "nd_int" [3] : i32
  %lt = sea.icmp slt, %a, %b : i32
  sea.cond_br %lt, ^bb1, ^bb2
^bb1:
  seaimp.set %x, %b : !seaimp.var<i32>
  sea.br ^bb2
^bb2:
  %m = seaimp.get %x : !seaimp.var<i32>
  %ok = sea.icmp sge, %m, %a : i32
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
