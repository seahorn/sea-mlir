// RUN: sea-vc %s --lower --print-refs=strong | FileCheck %s
// RUN: sea-vc %s --lower --seam --print-refs=strong | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --solver=yices --print-refs=strong | FileCheck %s %}
// RUN: %if yices %{ sea-vc %s --lower --seam --solver=yices --print-refs=strong | FileCheck %s %}
// RUN: sea-vc %s --lower --ackermannize=off --print-refs=strong | FileCheck %s
// RUN: %if yices %{ sea-vc %s --lower --seam --ackermannize=off --solver=yices --print-refs=strong | FileCheck %s %}
// RUN: sea-vc %s --lower --seam --print-refs=strong --format=jsonl | FileCheck %s --check-prefix=JSON

// Cells of the initial memory no constraint of the query reads are defaults
// of the model: `..` in a hex dump, `(default)` after a value read from
// them, `default` ranges in jsonl. The query reads one byte of buf (x); the
// rest of buf and all of other are defaults, whatever value the solver's
// model (an array `else`, the 0 of the Ackermann function) gives them. With
// --ackermannize=off the solvers' own array models answer.
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @defaults {
^bb0:
  %n = sea.const 8 : i64
  %m = sea.mem.nondet "region" [0] : !m loc("mem")
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64> loc("buf")
  %q = sea.alloca %n align 8 [2] : i64 -> !sea.ptr<64> loc("other")
  %x = sea.load %m[%p] align 1 : !m -> i8 loc("x")
  %c = sea.const 7 : i8
  %ok = sea.icmp ne, %x, %c : i8
  sea.print.mem "buf" = "mem"["buf"] : i8 {count = 8, fmt = "hexdump"}
  sea.print.mem "other" = "mem"["other"] : i8 {count = 8, fmt = "hexdump"}
  sea.print.mem "x0" = "mem"["buf"] : i8
  sea.print.mem "w" = "mem"["other"] : i32
  sea.print.mem "v" = "mem"["buf"] : i16
  sea.assert %ok
  sea.halt
}
// CHECK: result: sat
// CHECK: print: buf +00: 07 .. .. .. .. .. .. .. |........|
// CHECK: print: other +00: .. .. .. .. .. .. .. .. |........|
// CHECK-NEXT: print: x0 = 7 : ui8{{$}}
// CHECK-NEXT: print: w = {{[0-9]+}} : ui32 (default)
// CHECK-NEXT: print: v = {{[0-9]+}} : ui16 (default)
// CHECK-NEXT: validate: checked={{[0-9]+}} mismatches=0

// JSON: {"kind":"print.mem","path":"buf",{{.*}}"unknown":[],"default":{{\[\[}}1,8]]
// JSON: {"kind":"print.mem","path":"other",{{.*}}"unknown":[],"default":{{\[\[}}0,8]]
// JSON: {"kind":"print","path":"x0",
// JSON-NOT: "default"
// JSON-SAME: "text"
// JSON: {"kind":"print","path":"w",{{.*}}"default":true

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
