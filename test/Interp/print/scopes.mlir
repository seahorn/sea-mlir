// RUN: sea-run %s --input=0=3 | FileCheck %s
// RUN: sea-run %s --input=0=3 --print=tree | FileCheck %s --check-prefix=TREE
// RUN: sea-run %s --input=0=3 --print=off | FileCheck %s --check-prefix=OFF
// RUN: sea-run %s --input=0=3 --print-limit=4 | FileCheck %s --check-prefix=LIMIT
// RUN: sea-run %s --input=0=3 --print-locs | FileCheck %s --check-prefix=LOCS

// Scopes make dotted paths (no begin / end lines); a loop prints each time;
// an unmatched end is a note; the line limit counts what it dropped.
sea.program @main {
  %n = sea.nondet "nondet_u8" [0] : i8 loc("n")
  %z = sea.const 0 : i8
  %one = sea.const 1 : i8
  sea.print.scope "outer"
  sea.br ^loop(%z : i8)
^loop(%i : i8 loc("i")):
  sea.print.scope "it"
  sea.print "i" = "i" : i8
  sea.print.end "it"
  %next = sea.add %i, %one : i8
  %more = sea.icmp ult, %next, %n : i8
  sea.cond_br %more, ^loop(%next : i8), ^done
^done:
  sea.print.end "outer"
  sea.print.end "never"
  sea.print "after" = "n" : i8
  sea.halt
}
// CHECK: print: outer.it.i = 0 : ui8
// CHECK-NEXT: print: outer.it.i = 1 : ui8
// CHECK-NEXT: print: outer.it.i = 2 : ui8
// CHECK-NEXT: print.note: unmatched sea.print.end "never"
// CHECK-NEXT: print: after = 3 : ui8
// CHECK-NEXT: status: halted

// TREE: print: outer:
// TREE-NEXT: print:   it:
// TREE-NEXT: print:     i = 0 : ui8

// OFF-NOT: print:

// LIMIT: print: outer.it.i = 2 : ui8
// LIMIT-NEXT: print.note: unmatched
// LIMIT-NEXT: print.truncated: 1
// LIMIT-NEXT: status: halted

// LOCS: print: outer.it.i = 0 : ui8 at={{.*}}scopes.mlir:17:3

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
