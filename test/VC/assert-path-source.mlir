// assert-path entries of branches without a source location (an exporter
// left the branch unlocated, so its location is a position in this file).
// In a program with source locations, a sea.cond_br without one is shown
// at its condition's definition, else at the last op executed before it in
// its block, if either has a source location; otherwise it is left out (an
// unconditional branch decides nothing: left out too). A program without
// source locations keeps its .mlir positions.
// RUN: sea-run %s --program=resolve --input=0=2 | FileCheck %s --check-prefix=RES
// RUN: sea-run %s --program=resolve --input=0=2 --format=jsonl \
// RUN:   | FileCheck %s --check-prefix=RESJSON
// RUN: sea-vc %s --program=resolve --lower | FileCheck %s --check-prefix=RESVC
// RUN: sea-run %s --program=skip --input=0=2 | FileCheck %s --check-prefix=SKIP
// RUN: sea-run %s --program=plain --input=0=2 | FileCheck %s --check-prefix=PLAIN

// The first cond_br: its condition's line (p.c:5:9); the second: its
// condition is unlocated, the add before it is not (p.c:8:7).
// RES: status: assert-failed
// RES: assert-at: p.c:8:7{{$}}
// RES: assert-path: p.c:8:7 <- p.c:5:9{{$}}
// RES-NOT: .mlir:
// RESJSON: "kind":"status"
// RESJSON-SAME: "path":["p.c:8:7","p.c:5:9"]
// RESVC: replay: assert-failed{{$}}
// RESVC: replay: assert-path: p.c:8:7 <- p.c:5:9{{$}}
sea.program @resolve {
  %x = sea.nondet "nd_u8" [0] : i8 loc("p.c":3:11)
  %c4 = sea.const 4 : i8
  %lt = sea.icmp ult, %x, %c4 : i8 loc("p.c":5:9)
  sea.cond_br %lt, ^a, ^b
^a:
  sea.br ^chk
^b:
  sea.br ^chk
^chk:
  %c2 = sea.const 2 : i8
  %y = sea.add %x, %c2 : i8 loc("p.c":8:7)
  %eq = sea.icmp eq, %y, %c4 : i8
  sea.cond_br %eq, ^err, ^ok
^ok:
  sea.halt
^err:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// No source location for the branch, its condition or its block: no
// assert-path (the program has source locations, so a position in this
// file would be the only, and a misleading, entry).
// SKIP: status: assert-failed
// SKIP: assert-at: p.c:3:11{{$}}
// SKIP-NOT: assert-path:
sea.program @skip {
  %x = sea.nondet "nd_u8" [0] : i8 loc("p.c":3:11)
  sea.br ^chk
^chk:
  %c2 = sea.const 2 : i8
  %eq = sea.icmp eq, %x, %c2 : i8
  sea.cond_br %eq, ^err, ^ok
^ok:
  sea.halt
^err:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// No source locations at all: the .mlir positions are the source.
// PLAIN: status: assert-failed
// PLAIN: assert-path: {{.*}}assert-path-source.mlir:[[#@LINE+7]]:3 <- {{.*}}assert-path-source.mlir:[[#@LINE+3]]:3{{$}}
sea.program @plain {
  %x = sea.nondet "nd_u8" [0] : i8
  sea.br ^chk
^chk:
  %c2 = sea.const 2 : i8
  %eq = sea.icmp eq, %x, %c2 : i8
  sea.cond_br %eq, ^err, ^ok
^ok:
  sea.halt
^err:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
