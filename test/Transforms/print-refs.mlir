// RUN: sea-opt %s --mlir-print-debuginfo --pass-pipeline='builtin.module(sea.program(sea-print-strengthen))' 2>%t.err | FileCheck %s --check-prefix=STRONG
// RUN: FileCheck %s --check-prefix=WARN < %t.err
// RUN: sea-opt %s --mlir-print-debuginfo --pass-pipeline='builtin.module(sea.program(sea-print-strengthen,sea-print-weaken))' 2>/dev/null | FileCheck %s --check-prefix=WEAK
// RUN: sea-opt %s --mlir-print-debuginfo --pass-pipeline='builtin.module(sea.program(sea-print-strengthen,sea-print-weaken))' 2>/dev/null | sea-run - --input=0=5 | FileCheck %s --check-prefix=OUT

// sea-print-strengthen resolves names to the closest dominating value with
// that name (a memory name to the version current at the print);
// sea-print-weaken turns operands back into names (nd.<site> first; a
// fresh NameLoc for a value without a unique name).
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("x")
  %c8 = sea.const 8 : i64
  %m0 = sea.mem.nondet "heap" [1] : !m loc("mem")
  %p = sea.malloc %c8 align 8 [2] : i64 -> !sea.ptr<64> loc("p")
  %m1 = sea.store %m0[%p], %x inplace : !m, i8
  %y = sea.add %x, %x : i8 loc("x")
  %z = sea.add %y, %x : i8
  // STRONG: %[[Y:.*]] = sea.add %{{.*}} loc(#[[LOCX:.*]])
  // STRONG: sea.print "x" %[[Y]] : i8
  sea.print "x" = "x" : i8
  // STRONG: sea.print "site" %[[X0:[^ ]*]] : i8
  sea.print "site" = "nd.0" : i8
  // STRONG: sea.print.mem "m" %[[M1:.*]][%{{.*}}] : !sea.mem<!sea.ptr<64>, i8> -> i8
  sea.print.mem "m" = "mem"["p"] : i8
  // WARN: the name 'z' does not resolve
  // STRONG: sea.print "z" = "z" : i8
  sea.print "z" = "z" : i8
  sea.halt
}
// WEAK: sea.print "x" = "print.0" : i8
// WEAK: sea.print "site" = "nd.0" : i8
// WEAK: sea.print.mem "m" = "nd.1"["p"] : i8
// WEAK: sea.print "z" = "z" : i8
// OUT: print: x = 10 : ui8
// OUT-NEXT: print: site = 5 : ui8
// OUT-NEXT: print.object: heap.2 kind=heap size=8
// OUT-NEXT: print: m = 5 : ui8
// OUT-NEXT: print: z = <dangling: z>

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
