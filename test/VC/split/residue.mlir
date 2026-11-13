// Finding split targets when the hard part of the query is not an open
// alias question (docs/design/Split.md, "Finding targets").
//
// @ack: x is read twice from A, before and after a store to another
// object, so the two reads, and the two range checks on them (lines 10 and
// 20), are one term in the VC. The second check selects the row k read
// twice from C: those reads are left to the Ackermann lowering, with the
// check in an ite condition of their indices. No alias question is open;
// --split-candidates attributes the reads. Of two values with one term,
// the one whose uses reach the select's condition is listed (a split
// replaces the value of that op only; the compares reach it through the
// and), the copy on the same line (same=); the and before its two
// compares (covers).
// RUN: sea-vc %s --lower --program=ack --seam --split-candidates --print-lowered=%t.low.mlir > %t.out
// RUN: cat %t.out %t.low.mlir | FileCheck %s --check-prefix=ACK
// ACK: split.candidates: open-aliases=0 {{.*}} reads={{[1-9]}}
// ACK-NEXT: split.bool-candidate: name=[[G:%[0-9]+]] target=value open-aliases=0 addrs=0 reads=2 cond-reads=2 covers=2 same=[[G0:%[0-9]+]] loc=t.c:20:3
// ACK-NEXT: split.bool-candidate: name={{%[0-9]+}} {{.*}} covers=0 same={{%[0-9]+}} loc=t.c:20:7
// ACK-NEXT: split.bool-candidate: name={{%[0-9]+}} {{.*}} covers=0 same={{%[0-9]+}} loc=t.c:20:20
// ACK-NEXT: split.arith-candidates:
// --print-lowered: the program the VC encodes, with the VC's names.
// ACK: sea-vc --print-lowered
// ACK: [[G0]] = sea.and {{.*}} loc("t.c":10:3)
// ACK: [[G]] = sea.and {{.*}} loc("t.c":20:3)
// ACK-NEXT: sea.select [[G]],

// The printed program is input to sea-vc without --lower: the same names
// (a fixpoint).
// RUN: sea-vc %t.low.mlir --program=ack --seam --split-candidates --print-lowered=%t.low2.mlir > %t.out2
// RUN: cat %t.out2 %t.low2.mlir | FileCheck %s --check-prefix=ACK
// RUN: diff %t.low.mlir %t.low2.mlir
// RUN: not sea-vc %s --lower --program=ack --print-lowered=- 2>&1 | FileCheck %s --check-prefix=STDOUT
// STDOUT: --print-lowered needs a file

// --format=jsonl renders to the text lines.
// RUN: sea-vc %s --lower --program=ack --seam --split-candidates --format=jsonl > %t.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.out --jsonl %t.jsonl \
// RUN:   --require split-candidates:open_aliases,reads,same_object_open,memop_open \
// RUN:   --require split-bool-candidate:name,reads,cond_reads,covers,same

// Selectors by line: an ambiguous one lists every match with its op, type
// and column; one that finds nothing names the results of other types on
// the line, the nearest lines with results of the kind, or the files.
// RUN: not sea-vc %s --lower --program=ack --seam --split=@t.c:20 2>&1 | FileCheck %s --check-prefix=AMBIG
// AMBIG: split selector '@t.c:20' is ambiguous (3 matches
// AMBIG-NEXT: {{%[0-9]+}} sea.icmp i1 col=7
// AMBIG-NEXT: {{%[0-9]+}} sea.icmp i1 col=20
// AMBIG-NEXT: {{%[0-9]+}} sea.and i1 col=3 [vc.split-target]
// RUN: not sea-vc %s --lower --program=ack --seam --split=@t.c:21 2>&1 | FileCheck %s --check-prefix=NONE
// NONE: split selector '@t.c:21' finds no i1 value
// NONE-NEXT: on line 21, not of the kind: {{%[0-9]+}} sea.select i64 col=3
// NONE-NEXT: nearest lines with i1 values (line:count): 10:3 20:3
// RUN: not sea-vc %s --lower --program=ack --seam --split=@x.c:1 2>&1 | FileCheck %s --check-prefix=FILE
// FILE: no op has a position in a file ending with 'x.c' (files: {{.*}}t.c)

// An unknown (z3 rlimit 1) with no open alias question but Ackermann
// constraints points to --split-candidates too; @same has neither.
// RUN: not sea-vc %s --lower --program=ack --seam --z3-param=rlimit=1 2>/dev/null | FileCheck %s --check-prefix=HINT
// RUN: not sea-vc %s --lower --program=ack --seam --z3-param=rlimit=1 --format=jsonl 2>/dev/null | FileCheck %s --check-prefix=HINTJ
// RUN: not sea-vc %s --lower --program=same --seam --z3-param=rlimit=1 2>/dev/null | FileCheck %s --check-prefix=NOHINT
// HINT: result: unknown
// HINT: hint: split: open-aliases=0 ackermann-constraints={{[1-9][0-9]*}}; {{.*}}--split-candidates
// HINTJ: "kind":"hint","topic":"split","open_aliases":0,"ackermann_constraints":{{[1-9]}}
// NOHINT: result: unknown
// NOHINT-NOT: hint:

// @same: a one-object alias question reduces to an offset compare
// (i = j). seam.alias.open.all counts it with the IS_OVERLAP questions;
// --split-candidates' open-aliases counts only the IS_OVERLAP ones and
// shows it as same-object-open.
// RUN: sea-vc %s --lower --program=same --seam --seam-stats --vcgen-only | FileCheck %s --check-prefix=STATS
// RUN: sea-vc %s --lower --program=same --seam --split-candidates | FileCheck %s --check-prefix=SAME
// STATS: seam.alias.open.all: 1
// SAME: split.candidates: open-aliases=0 {{.*}} same-object-open=1 memop-open=0

!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ack {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %C = sea.malloc %c16 align 8 [3] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [4] : !m
  %z = sea.const 0 : i64
  %n = sea.const 8 : i64
  %one = sea.const 1 : i64
  %x0 = sea.load %m0[%A] : !m -> i64
  %lo0 = sea.icmp sge, %x0, %z : i64 loc("t.c":10:7)
  %hi0 = sea.icmp slt, %x0, %n : i64 loc("t.c":10:20)
  %g0 = sea.and %lo0, %hi0 : i1 loc("t.c":10:3)
  %v = sea.const 5 : i8
  %m1 = sea.store %m0[%B], %v inplace : !m, i8
  %x1 = sea.load %m1[%A] : !m -> i64
  %lo1 = sea.icmp sge, %x1, %z : i64 loc("t.c":20:7)
  %hi1 = sea.icmp slt, %x1, %n : i64 loc("t.c":20:20)
  %g1 = sea.and %lo1, %hi1 : i1 loc("t.c":20:3)
  %k = sea.select %g1, %x1, %z : i64 loc("t.c":21:3)
  %p = sea.ptr.add %C, %k : !sea.ptr<64>, i64
  %y0 = sea.load %m1[%p] : !m -> i8
  %p1 = sea.ptr.add %p, %one : !sea.ptr<64>, i64
  %y1 = sea.load %m1[%p1] : !m -> i8
  %d = sea.icmp ne, %y0, %y1 : i8
  %ng = sea.xor %g0, %d : i1
  sea.assert %ng
  sea.halt
}
sea.program @same {
  %c16 = sea.const 16 : i64
  %A = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [2] : !m
  %i = sea.nondet "i" [3] : i64
  %j = sea.nondet "j" [4] : i64
  %p = sea.ptr.add %A, %i : !sea.ptr<64>, i64
  %q = sea.ptr.add %A, %j : !sea.ptr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %x = sea.load %m1[%q] : !m -> i8
  %ok = sea.icmp ne, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
