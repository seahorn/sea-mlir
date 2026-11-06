// RUN: sea-run %s --input=0=0 | FileCheck %s
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sccp,canonicalize))' --mlir-print-debuginfo \
// RUN:   | sea-run - --input=0=0 | FileCheck %s --check-prefix=OPT
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-print-strengthen,sccp,canonicalize))' --mlir-print-debuginfo \
// RUN:   | sea-run - --input=0=0 | FileCheck %s --check-prefix=STRONG

// Weak prints peek a name; no definition on the executed path (or one of
// another type) prints <dangling: name>, never a guess. A pass may remove
// the value (sccp folds %k, canonicalize drops it): the weak print then
// dangles; strengthened first, the print keeps it alive (an operand: the
// folded constant).
sea.program @main {
  %c = sea.nondet "nondet_bool" [0] : i1
  %two = sea.const 2 : i8
  %k = sea.add %two, %two : i8 loc("k")
  sea.cond_br %c, ^then, ^else
^then:
  %t = sea.const 7 : i8 loc("t")
  sea.br ^join
^else:
  sea.br ^join
^join:
  // CHECK: print: k = 4 : ui8
  // OPT: print: k = <dangling: k>
  // STRONG: print: k = 4 : ui8
  sea.print "k" = "k" : i8
  // CHECK: print: t = <dangling: t>
  sea.print "t" = "t" : i8
  // CHECK: print: wide = <dangling: k>
  sea.print "wide" = "k" : i32
  // CHECK: print: none = <dangling: nosuch>
  sea.print "none" = "nosuch" : i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
