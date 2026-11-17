// RUN: sea-vc %s --program=unr --lower 2>&1 | FileCheck %s --check-prefix=UNR
// RUN: sea-vc %s --program=unr --lower --lower-disable=prune-edges,assume-fold 2>&1 | FileCheck %s --check-prefix=UNR-OFF
// RUN: sea-vc %s --program=unr --lower --lower-disable=assume-fold 2>&1 | FileCheck %s --check-prefix=UNR-PRUNE
// RUN: sea-vc %s --program=unr --lower --reach 2>&1 | FileCheck %s --check-prefix=UNR-REACH
// RUN: sea-vc %s --program=keep --lower --replay-original --print-lowered=%t.keep.mlir 2>&1 | FileCheck %s --check-prefix=KEEP
// RUN: FileCheck %s --check-prefix=KEEP-LOW < %t.keep.mlir
// RUN: sea-vc %s --program=cont_dead --lower --replay-original --print-lowered=%t.cont.mlir 2>&1 | FileCheck %s --check-prefix=CONT
// RUN: FileCheck %s --check-prefix=CONT-LOW < %t.cont.mlir
// sea-prune-edges in the lowering (docs/design/PruneEdges.md).
// @unr: the branch into a block that is only sea.unreachable becomes an
// assume of the other side; the false edge into the join's `assume %ok`
// is dead, so is the arm before it; the assume-fold rounds then block
// every path: statically safe. The edge facts of sea-assume-fold settle
// this program too; with both fact folds off (and under --reach, which
// decides prefixes of executions) the branches stay, and the pass alone
// (assume-fold off: no rounds of its rewrite) prunes both edges.
// UNR: vc.prune-edges: edges={{[1-9]}}
// UNR: vc.statically-safe:
// UNR: result: unsat
// UNR-OFF-NOT: vc.prune-edges
// UNR-OFF-NOT: vc.statically-safe
// UNR-OFF: result: unsat
// UNR-PRUNE: vc.prune-edges: edges=2
// UNR-REACH-NOT: vc.prune-edges
sea.program @unr {
  %c5 = sea.const 5 : i8
  %f = sea.const 0 : i1
  %x = sea.nondet "x" [0] : i8
  %y = sea.nondet "y" [1] : i8
  %c = sea.icmp ult, %x, %c5 : i8
  sea.cond_br %c, ^dead, ^live
^dead:
  sea.unreachable
^live:
  sea.cond_br %c, ^a, ^b
^a:
  %g = sea.icmp eq, %y, %c5 : i8
  sea.assert %g
  sea.halt
^b:
  sea.br ^j(%f : i1)
^b2:
  %t = sea.icmp ugt, %y, %c5 : i8
  sea.br ^j(%t : i1)
^j(%ok : i1):
  sea.assume %ok
  %h = sea.icmp ne, %y, %c5 : i8
  sea.assert %h
  sea.halt
}

// @keep: the join's other incoming value is not a constant: that edge
// stays, the failure behind it is found and named (the held assert
// before it is no failure site).
// KEEP: vc.prune-edges: edges=1
// KEEP: result: sat
// KEEP: replay: assert-op: x.c:9:3
// KEEP: replay.original: status=assert-failed at=x.c:9:3 match=yes
// KEEP-LOW-NOT: sea.cond_br {{.*}}"x.c":4:3
// KEEP-LOW: sea.assume {{.*}}"x.c":4:3
// KEEP-LOW: sea.assume {{.*}}"x.c":7:3
sea.program @keep {
  %f = sea.const 0 : i1
  %c10 = sea.const 10 : i8
  %x = sea.nondet "x" [0] : i8
  %y = sea.nondet "y" [1] : i8
  %c = sea.nondet "c" [2] : i1
  %p = sea.icmp ult, %x, %c10 : i8
  sea.assume %p loc("h.c":2:3)
  sea.assert %p loc("x.c":3:3)
  sea.cond_br %c, ^a, ^b loc("x.c":4:3)
^a:
  sea.br ^j(%f : i1) loc("x.c":5:3)
^b:
  %t = sea.icmp ugt, %y, %x : i8
  sea.br ^j(%t : i1) loc("x.c":6:3)
^j(%ok : i1):
  sea.assume %ok loc("x.c":7:3)
  %q = sea.icmp ne, %y, %c10 : i8
  sea.assert %q loc("x.c":9:3)
  sea.halt
}

// @cont_dead: past the merged assert at x.c:3 every path blocks (the
// join's assume of `false`), so its branch becomes `assume !p` and the
// marked branch into its landing block: a replay names that assert, as
// the original program fails it.
// CONT: vc.prune-edges: edges=1
// CONT: result: sat
// The failing condition is still found (assert-at: the icmp, not the
// located branch between it and the assert; the interpreter reads it from
// the `assume !p` before the marked br).
// CONT: replay: assert-at: x.c:2:3
// CONT: replay: assert-op: x.c:3:3
// CONT: replay.original: status=assert-failed at=x.c:3:3 match=yes
// CONT-LOW-NOT: sea.cond_br {{.*}}"x.c":3:3
// CONT-LOW: sea.br {{.*}}loc("sea.assert"("x.c":3:3))
sea.program @cont_dead {
  %f = sea.const 0 : i1
  %c10 = sea.const 10 : i8
  %x = sea.nondet "x" [0] : i8
  %y = sea.nondet "y" [1] : i8
  %p = sea.icmp ult, %x, %c10 : i8 loc("x.c":2:3)
  sea.br ^chk loc("x.c":2:9)
^chk:
  sea.assert %p loc("x.c":3:3)
  sea.br ^exit(%f : i1) loc("x.c":4:3)
^exit(%ok : i1):
  sea.assume %ok loc("x.c":5:3)
  %q = sea.icmp ne, %y, %c10 : i8
  sea.assert %q loc("x.c":6:3)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
