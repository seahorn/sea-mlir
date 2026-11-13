// RUN: sea-vc %s --program=empty_keeps_path --lower | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=step --lower | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --program=step_out --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=step_edge --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=two --lower | FileCheck %s --check-prefix=SAT2
// RUN: sea-vc %s --program=two_out --lower | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --program=expanded --lower --replay-original | FileCheck %s --check-prefix=EXPANDED
// RUN: sea-vc %s --program=expanded_out --lower | FileCheck %s --check-prefix=EXPANDED-OUT
// The lowering of sea.box.all against its semantics (docs/design/BoxAll.md:
// unsigned, upper bound exclusive, lower bound and step constants, an empty
// box is true), on boxes whose upper bound is a value (the guarded form).
// Each sat replays on the interpreter, which evaluates the whole box.
// SAT: vc.box-all: {{.*}}guarded=1
// SAT: result: sat
// SAT: replay: assert-failed
// SAT: validate: {{.*}} mismatches=0
// SAT2: result: sat
// SAT2: callee=box.all.b0.0 width=64 value=1
// SAT2: callee=box.all.b0.1 width=64 value=2
// SAT2: replay: assert-failed
// UNSAT: result: unsat
// EXPANDED: vc.box-all: {{.*}}expanded=1
// EXPANDED: result: sat
// EXPANDED: replay: assert-failed
// EXPANDED: validate: {{.*}} mismatches=0
// EXPANDED: replay.original: status=assert-failed {{.*}}match=yes {{.*}}box-all-false=(3, 2)
// EXPANDED-OUT: vc.box-all: {{.*}}expanded=1
// EXPANDED-OUT: result: unsat

// The box is empty (n = 0): the claim is true and the path goes on to the
// rest of the assertion, which fails for x = 3. An assume(t ult n) in the
// lowering would block this path (unsat).
sea.program @empty_keeps_path {
  %n = sea.nondet "n" [0] : i64
  %x = sea.nondet "x" [1] : i64
  %c0 = sea.const 0 : i64
  %c3 = sea.const 3 : i64
  %z = sea.icmp eq, %n, %c0 : i64
  sea.assume %z
  %all = sea.box.all (%i) = (0) to (%n) {
    %f = sea.const 0 : i1
    sea.yield %f
  }
  %ne = sea.icmp ne, %x, %c3 : i64
  %ok = sea.and %all, %ne : i1
  sea.assert %ok
  sea.halt
}

// Indices 3, 5, ..., below n = 10: 9 is one of them.
sea.program @step {
  %n = sea.nondet "n" [0] : i64
  %x = sea.nondet "x" [1] : i64
  %c9 = sea.const 9 : i64
  %c10 = sea.const 10 : i64
  %a = sea.icmp eq, %x, %c9 : i64
  %b = sea.icmp eq, %n, %c10 : i64
  %ab = sea.and %a, %b : i1
  sea.assume %ab
  %all = sea.box.all (%k) = (3) to (%n) step (2) {
    %e = sea.icmp ne, %k, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// 4 is not an index (odd ones from 3), whatever n.
sea.program @step_out {
  %n = sea.nondet "n" [0] : i64
  %x = sea.nondet "x" [1] : i64
  %c4 = sea.const 4 : i64
  %a = sea.icmp eq, %x, %c4 : i64
  sea.assume %a
  %all = sea.box.all (%k) = (3) to (%n) step (2) {
    %e = sea.icmp ne, %k, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// The upper bound is exclusive: 9 is not below n = 9; nor is any index
// with n <= 3 (empty).
sea.program @step_edge {
  %n = sea.nondet "n" [0] : i64
  %x = sea.nondet "x" [1] : i64
  %c9 = sea.const 9 : i64
  %a = sea.icmp eq, %x, %c9 : i64
  %b = sea.icmp ule, %n, %c9 : i64
  %ab = sea.and %a, %b : i1
  sea.assume %ab
  %all = sea.box.all (%k) = (3) to (%n) step (2) {
    %e = sea.icmp ne, %k, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// (i, j) in [0, n) x [0, 3), n = 2: 3i + j = 5 at (1, 2).
sea.program @two {
  %n = sea.nondet "n" [0] : i64
  %x = sea.nondet "x" [1] : i64
  %c2 = sea.const 2 : i64
  %c3 = sea.const 3 : i64
  %c5 = sea.const 5 : i64
  %a = sea.icmp eq, %x, %c5 : i64
  %b = sea.icmp eq, %n, %c2 : i64
  %ab = sea.and %a, %b : i1
  sea.assume %ab
  %all = sea.box.all (%i, %j) = (0, 0) to (%n, %c3) {
    %r = sea.mul %i, %c3 : i64
    %v = sea.add %r, %j : i64
    %e = sea.icmp ne, %v, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// n = 1: 3i + j covers 0..2 only.
sea.program @two_out {
  %n = sea.nondet "n" [0] : i64
  %x = sea.nondet "x" [1] : i64
  %c1 = sea.const 1 : i64
  %c3 = sea.const 3 : i64
  %c5 = sea.const 5 : i64
  %a = sea.icmp eq, %x, %c5 : i64
  %b = sea.icmp eq, %n, %c1 : i64
  %ab = sea.and %a, %b : i1
  sea.assume %ab
  %all = sea.box.all (%i, %j) = (0, 0) to (%n, %c3) {
    %r = sea.mul %i, %c3 : i64
    %v = sea.add %r, %j : i64
    %e = sea.icmp ne, %v, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}


// A constant 4 x 3 box (12 indices <= the default --box-all-expand):
// expanded. 10i + j = 32 at (3, 2); 33 is not in the box (j < 3).
sea.program @expanded {
  %x = sea.nondet "x" [0] : i64
  %c3 = sea.const 3 : i64
  %c4 = sea.const 4 : i64
  %c10 = sea.const 10 : i64
  %c32 = sea.const 32 : i64
  %a = sea.icmp eq, %x, %c32 : i64
  sea.assume %a
  %all = sea.box.all (%i, %j) = (0, 0) to (%c4, %c3) {
    %r = sea.mul %i, %c10 : i64
    %v = sea.add %r, %j : i64
    %e = sea.icmp ne, %v, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

sea.program @expanded_out {
  %x = sea.nondet "x" [0] : i64
  %c3 = sea.const 3 : i64
  %c4 = sea.const 4 : i64
  %c10 = sea.const 10 : i64
  %c33 = sea.const 33 : i64
  %a = sea.icmp eq, %x, %c33 : i64
  sea.assume %a
  %all = sea.box.all (%i, %j) = (0, 0) to (%c4, %c3) {
    %r = sea.mul %i, %c10 : i64
    %v = sea.add %r, %j : i64
    %e = sea.icmp ne, %v, %x : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
