// assert-at names the definition of the failing assert's condition, or of
// its first false conjunct through a chain of sea.and; under
// sea-merge-asserts the condition of the original assert that failed. Ops
// after the condition (pointer arithmetic, prints) are not the location.
// RUN: sea-vc %s --program=straight --lower | FileCheck %s --check-prefix=STRAIGHT
// RUN: sea-run %s --program=straight --input=0=7 | FileCheck %s --check-prefix=RSTRAIGHT
// RUN: sea-vc %s --program=conj --lower | FileCheck %s --check-prefix=CONJ
// RUN: sea-run %s --program=conj --input=0=200 | FileCheck %s --check-prefix=RCONJ
// RUN: sea-vc %s --program=merged --lower | FileCheck %s --check-prefix=MERGED
// RUN: sea-run %s --program=printlast --input=0=1 | FileCheck %s --check-prefix=PRINTLAST

// STRAIGHT: replay: assert-failed{{$}}
// STRAIGHT-NEXT: replay: assert-at: c.c:10:7{{$}}
// STRAIGHT: replay: assert-op: c.c:12:3{{$}}
// STRAIGHT: validate: checked={{[0-9]+}} mismatches=0
// RSTRAIGHT: status: assert-failed
// RSTRAIGHT: assert-at: c.c:10:7{{$}}
sea.program @straight {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("c.c":3:11)
  %c7 = sea.const 7 : i8
  %ok = sea.icmp ne, %x, %c7 : i8 loc("c.c":10:7)
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64> loc("c.c":11:3)
  %one = sea.const 1 : i64
  %q = sea.ptr.add %p, %one : !sea.ptr<64>, i64 loc("c.c":11:20)
  sea.print "x" = "nd.0" : i8 loc("c.c":11:30)
  sea.assert %ok loc("c.c":12:3)
  sea.halt
}

// CONJ: replay: assert-failed{{$}}
// CONJ-NEXT: replay: assert-at: c.c:22:7{{$}}
// RCONJ: assert-at: c.c:22:7{{$}}
sea.program @conj {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("c.c":20:11)
  %c3 = sea.const 3 : i8
  %c100 = sea.const 100 : i8
  %a = sea.icmp uge, %x, %c3 : i8 loc("c.c":21:7)
  %b = sea.icmp ult, %x, %c100 : i8 loc("c.c":22:7)
  %ab = sea.and %a, %b : i1 loc("c.c":23:7)
  %t = sea.const 1 : i1
  %all = sea.and %t, %ab : i1 loc("c.c":24:7)
  %lo = sea.const 5 : i8
  %gt = sea.icmp ugt, %x, %lo : i8
  sea.assume %gt
  sea.assert %all loc("c.c":25:3)
  sea.halt
}

// Two asserts merged; the second fails (the first holds by the
// assumption), its condition a conjunction whose second conjunct is false.
// MERGED: replay: assert-failed{{$}}
// MERGED-NEXT: replay: assert-at: c.c:33:7{{$}}
// MERGED: replay: assert-op: c.c:35:3{{$}}
sea.program @merged {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("c.c":30:11)
  %c9 = sea.const 9 : i8
  %c4 = sea.const 4 : i8
  %ne9 = sea.icmp ne, %x, %c9 : i8 loc("c.c":31:7)
  %ne9a = sea.icmp ne, %x, %c9 : i8
  sea.assume %ne9a
  sea.assert %ne9 loc("c.c":31:3)
  %pos = sea.icmp ne, %x, %c9 : i8 loc("c.c":32:7)
  %ne4 = sea.icmp ne, %x, %c4 : i8 loc("c.c":33:7)
  %both = sea.and %pos, %ne4 : i1 loc("c.c":34:7)
  sea.assert %both loc("c.c":35:3)
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64> loc("c.c":36:3)
  sea.halt
}

// `assert false` reached by a branch: the branch, not the print op after it.
// PRINTLAST: status: assert-failed
// PRINTLAST: assert-at: c.c:40:3{{$}}
sea.program @printlast {
  %x = sea.nondet "nondet_u8" [0] : i8 loc("c.c":39:11)
  %c1 = sea.const 1 : i8
  %ne = sea.icmp ne, %x, %c1 : i8 loc("c.c":40:7)
  sea.cond_br %ne, ^ok, ^err loc("c.c":40:3)
^ok:
  sea.halt
^err:
  sea.print "x" = "nd.0" : i8 loc("c.c":41:3)
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
