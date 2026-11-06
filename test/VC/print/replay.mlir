// RUN: sea-vc %s --lower | FileCheck %s --check-prefixes=CHECK,KEPT,DEAD
// RUN: sea-vc %s --lower --seam | FileCheck %s --check-prefixes=CHECK,KEPT,DEAD
// RUN: sea-vc %s --lower --widen-memory=8 | FileCheck %s --check-prefixes=CHECK,KEPT,DEAD
// RUN: sea-vc %s --lower --lower-opt=false | FileCheck %s --check-prefixes=CHECK,KEPT,LIVE
// RUN: sea-vc %s --lower --print-refs=strong | FileCheck %s --check-prefixes=CHECK,KEPT,LIVE
// RUN: sea-vc %s --lower --print-refs=weak | FileCheck %s --check-prefixes=CHECK,WEAK,DEAD
// RUN: sea-vc %s --lower --print=off | FileCheck %s --check-prefix=OFF

// sea-vc replays a sat model and prints the program's prints, after the
// replay status (the `replay:` status line stays first and exact) and
// before `validate:`. Pointers are object + offset after placement too;
// bytes the model leaves unconstrained are `..` / (default). The struct
// print is strong as written: its leaves survive sea-scalarize-structs.
// `end` has no use but a weak print: the lowering's dead-code erasure
// (--lower-opt) removes it and the print dangles (DEAD); a strong print
// keeps it, and so does --lower-opt=false (LIVE).
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @main {
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %c100 = sea.const 100 : i64
  %m0 = sea.mem.nondet "heap" [1] : !m loc("mem")
  %a = sea.malloc %c16 align 8 [2] label "node a" : i64 -> !sea.ptr<64> loc("a")
  %b = sea.malloc %c16 align 8 [3] label "node b" : i64 -> !sea.ptr<64> loc("b")
  %v = sea.nondet "nondet_u64" [4] : i64 loc("v")
  %an = sea.ptr.add %a, %c8 : !sea.ptr<64>, i64 loc("an")
  %m1 = sea.store %m0[%a], %v inplace align 8 : !m, i64
  %m2 = sea.store %m1[%an], %b inplace align 8 : !m, !sea.ptr<64>
  %end = sea.ptr.add %a, %c16 : !sea.ptr<64>, i64 loc("end")
  %s = sea.struct.create %v, %an : i64, !sea.ptr<64> -> !sea.struct<i64, !sea.ptr<64>> loc("s")
  %w = sea.load %m2[%a] align 8 : !m -> i64 loc("w")
  sea.print.scope "a"
  sea.print.mem "val" = "mem"["a"] : i64 {fmt = "s"}
  sea.print.mem "next" = "mem"["a"] : !sea.ptr<64> {offset = 8}
  sea.print.end "a"
  sea.print "s" %s : !sea.struct<i64, !sea.ptr<64>> {names = ["val", "next"]}
  sea.print "end" = "end" : !sea.ptr<64>
  sea.print "b" = "b" : !sea.ptr<64>
  sea.print.mem "bval" = "mem"["b"] : i64
  sea.print.mem "dump" = "mem"["a"] : i8 {fmt = "hexdump", count = 32}
  %ok = sea.icmp ult, %w, %c100 : i64
  sea.assert %ok
  sea.halt
}
// CHECK: result: sat
// CHECK: replay: assert-failed
// CHECK: print: a.val = [[V:[0-9]+]] : si64
// CHECK: print.object: heap.3 kind=heap size=16 label=node b
// CHECK-NEXT: print: a.next = &heap.3[0]
// KEPT: print: s.val = [[V]] : ui64
// KEPT-NEXT: print: s.next = &heap.2[8]
// a weak print of a struct value: sea-scalarize-structs removed it
// WEAK: print: s = <dangling: s>
// LIVE-NEXT: print: end = &heap.2[16] end
// DEAD-NEXT: print: end = <dangling: end>
// CHECK-NEXT: print: b = &heap.3[0]
// CHECK-NEXT: print: bval = 0 : ui64 (default)
// CHECK-NEXT: print: dump = heap.2[0..32]
// CHECK-NEXT: print: dump +00: {{([0-9a-f][0-9a-f] )+}}10 80 04 08 00 00 00 00 |
// CHECK-NEXT: print: dump +10: .. .. .. .. .. .. .. .. .. .. .. .. .. .. .. .. |................|
// CHECK-NEXT: validate: checked={{[0-9]+}} mismatches=0

// OFF: replay: assert-failed
// OFF-NOT: print:
// OFF: validate:

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
