// Which assertion failed: SeaHorn merges every assertion into one
// `sea.assert false` in an error block, so the replay names the source
// location of the last executed branch (here the check that jumped to the
// error block), the assert's own location, and the recent branch path.
// The location is that of the last located op executed before the assert
// (a branch, or the computation of a condition): positions in this .mlir
// file (ops without an explicit loc) only if there is no source location.
// Two checks of foo.c (lines 12 and 14) share the error block; each
// program's assumption makes exactly one of them fail.
// RUN: sea-vc %s --program=second --lower | FileCheck %s --check-prefix=SECOND
// RUN: sea-vc %s --program=first --lower | FileCheck %s --check-prefix=FIRST
// RUN: sea-run %s --program=second --input=0=7 | FileCheck %s --check-prefix=RUN2
// RUN: sea-run %s --program=second --input=0=9 | FileCheck %s --check-prefix=RUNOK
// RUN: sea-run %s --program=first --input=0=5 | FileCheck %s --check-prefix=RUN1
// RUN: sea-vc %s --program=sassert --lower | FileCheck %s --check-prefix=SASSERT

// SECOND: result: sat
// SECOND: replay: assert-failed{{$}}
// SECOND-NEXT: replay: assert-at: foo.c:14:3{{$}}
// SECOND: replay: assert-op: foo.c:20:5{{$}}
// SECOND: replay: assert-path: foo.c:14:3 <- foo.c:12:3 <- foo.c:4:3{{$}}
// SECOND: validate: checked={{[0-9]+}} mismatches=0
sea.program @second {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("foo.c":3:11)
  %c6 = sea.const 6 : i8
  %big = sea.icmp uge, %x, %c6 : i8
  sea.assume %big loc("foo.c":4:3)
  sea.br ^chk1 loc("foo.c":4:3)
^chk1:
  %c5 = sea.const 5 : i8
  %ne5 = sea.icmp ne, %x, %c5 : i8 loc("foo.c":12:10)
  sea.cond_br %ne5, ^chk2, ^err loc("foo.c":12:3)
^chk2:
  %c7 = sea.const 7 : i8
  %ne7 = sea.icmp ne, %x, %c7 : i8 loc("foo.c":14:10)
  sea.cond_br %ne7, ^ok, ^err loc("foo.c":14:3)
^ok:
  sea.halt loc("foo.c":16:1)
^err:
  %f = sea.const 0 : i1
  sea.assert %f loc("foo.c":20:5)
  sea.halt
}

// FIRST: result: sat
// FIRST: replay: assert-failed{{$}}
// FIRST-NEXT: replay: assert-at: foo.c:12:3{{$}}
// FIRST: replay: assert-op: foo.c:20:5{{$}}
// FIRST: replay: assert-path: foo.c:12:3{{$}}
sea.program @first {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("foo.c":3:11)
  %c6 = sea.const 6 : i8
  %small = sea.icmp ult, %x, %c6 : i8
  sea.assume %small
  %c5 = sea.const 5 : i8
  %ne5 = sea.icmp ne, %x, %c5 : i8 loc("foo.c":12:10)
  sea.cond_br %ne5, ^chk2, ^err loc("foo.c":12:3)
^chk2:
  %c7 = sea.const 7 : i8
  %ne7 = sea.icmp ne, %x, %c7 : i8 loc("foo.c":14:10)
  sea.cond_br %ne7, ^ok, ^err loc("foo.c":14:3)
^ok:
  sea.halt
^err:
  %f = sea.const 0 : i1
  sea.assert %f loc("foo.c":20:5)
  sea.halt
}

// RUN2: status: assert-failed
// RUN2: assert-at: foo.c:14:3{{$}}
// RUN2: assert-op: foo.c:20:5{{$}}
// RUN2: assert-path: foo.c:14:3 <- foo.c:12:3 <- foo.c:4:3{{$}}
// RUNOK: status: halted
// RUNOK-NOT: assert-
// RUN1: status: assert-failed
// RUN1: assert-at: foo.c:12:3{{$}}
// RUN1: assert-path: foo.c:12:3{{$}}

// SeaHorn's sassert shape: `assume !cond; br; assert false`, the error
// branches unlocated: the condition's line, not the last located branch.
// SASSERT: result: sat
// SASSERT: replay: assert-failed{{$}}
// SASSERT-NEXT: replay: assert-at: bar.c:30:3{{$}}
// SASSERT: replay: assert-path: bar.c:25:7{{$}}
sea.program @sassert {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("bar.c":24:11)
  %c3 = sea.const 3 : i8
  %small = sea.icmp ult, %x, %c3 : i8 loc("bar.c":25:7)
  sea.cond_br %small, ^a, ^b loc("bar.c":25:7)
^a:
  sea.br ^chk(%x : i8)
^b:
  sea.br ^chk(%c3 : i8)
^chk(%y : i8):
  %ok = sea.icmp ult, %y, %c3 : i8 loc("bar.c":30:3)
  %t = sea.const 1 : i1 loc("bar.c":30:3)
  %bad = sea.xor %ok, %t : i1 loc("bar.c":30:3)
  sea.assume %bad
  sea.br ^err
^err:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
