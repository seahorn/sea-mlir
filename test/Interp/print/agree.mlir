// RUN: sea-run %s --input=4=1 | FileCheck %s
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-scalarize-structs,sea-place-static,sea-split-wide-access))' --mlir-print-debuginfo \
// RUN:   | sea-run - --input=4=1 | FileCheck %s --check-prefixes=CHECK,PLACED

// The same pointer tokens before and after placement (and after
// sea-split-wide-access splits pointer stores / loads into byte cells):
// placed addresses carry their object (provenance), through address
// arithmetic, selects, block arguments and memory. The SeaHorn layout puts
// heap.2 and heap.3 back to back, so one past the end of heap.2 is the
// base of heap.3: provenance tells them apart.
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %c8 = sea.const 8 : i64
  %c16 = sea.const 16 : i64
  %m0 = sea.mem.nondet "heap" [1] : !m loc("mem")
  %a = sea.malloc %c16 align 8 [2] label "a" : i64 -> !sea.ptr<64> loc("a")
  %b = sea.malloc %c16 align 8 [3] label "b" : i64 -> !sea.ptr<64> loc("b")
  %c = sea.nondet "nondet_bool" [4] : i1
  %a8 = sea.ptr.add %a, %c8 : !sea.ptr<64>, i64 loc("a8")
  %aend = sea.ptr.add %a, %c16 : !sea.ptr<64>, i64 loc("aend")
  %m1 = sea.store %m0[%a8], %b inplace : !m, !sea.ptr<64>
  %back = sea.load %m1[%a8] : !m -> !sea.ptr<64> loc("back")
  %sel = sea.select %c, %a8, %b : !sea.ptr<64> loc("sel")
  sea.br ^next(%aend : !sea.ptr<64>)
^next(%arg : !sea.ptr<64> loc("arg")):
  // PLACED: print.object: heap.2 kind=heap size=16 label=a {{.*}}base=0x8048000
  // CHECK: print: a8 = &heap.2[8]
  sea.print "a8" = "a8" : !sea.ptr<64>
  // CHECK: print: aend = &heap.2[16] end
  sea.print "aend" = "aend" : !sea.ptr<64>
  // PLACED: print.object: heap.3 kind=heap size=16 label=b {{.*}}base=0x8048010
  // CHECK: print: b = &heap.3[0]
  sea.print "b" = "b" : !sea.ptr<64>
  // CHECK: print: back = &heap.3[0]
  sea.print "back" = "back" : !sea.ptr<64>
  // CHECK: print: sel = &heap.2[8]
  sea.print "sel" = "sel" : !sea.ptr<64>
  // CHECK: print: arg = &heap.2[16] end
  sea.print "arg" = "arg" : !sea.ptr<64>
  // CHECK: print: stored = &heap.3[0]
  sea.print.mem "stored" = "mem"["a"] : !sea.ptr<64> {offset = 8}
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
