// A reach job (SeaHorn export, all checks off): the reach condition is a
// path condition decided in main, and an inlined callee (verify) runs
// after the decision, before the program's one `assert false`.
// The exporter can locate the branch into the error block and the assert
// at the sassert that failed (the KIT_REACH line, main.c:33:3): then
// assert-at and assert-op name it, not the callee's last op.
// RUN: sea-vc %s --program=located --lower | FileCheck %s --check-prefix=LOC
// RUN: sea-run %s --program=located --input=0=7 | FileCheck %s --check-prefix=INTERP
// Without those locations (the error branch folded into its predecessor
// and the assert unlocated), nothing in the program names the reach line:
// assert-at falls back to the last located op, inside the callee.
// RUN: sea-vc %s --program=unlocated --lower | FileCheck %s --check-prefix=UNLOC

// LOC: result: sat
// LOC: replay: assert-failed{{$}}
// LOC-NEXT: replay: assert-at: main.c:33:3{{$}}
// LOC: replay: assert-op: main.c:33:3{{$}}
// LOC: replay: assert-path: main.c:33:3 <- main.c:30:7{{$}}
// INTERP: status: assert-failed
// INTERP: assert-at: main.c:33:3{{$}}
// INTERP: assert-op: main.c:33:3{{$}}
sea.program @located {
  %x = sea.nondet "nd_u8" [0] : i8 loc("main.c":20:11)
  %c5 = sea.const 5 : i8 loc("main.c":30:11)
  %big = sea.icmp ugt, %x, %c5 : i8 loc("main.c":30:11)
  sea.cond_br %big, ^hit, ^miss loc("main.c":30:7)
^miss:
  sea.unreachable
^hit:
  %c9 = sea.const 9 : i8 loc(callsite("verify.c":12:15 at "main.c":31:7))
  %ok = sea.icmp ult, %x, %c9 : i8 loc(callsite("verify.c":12:15 at "main.c":31:7))
  sea.assume %ok loc(callsite("verify.c":12:3 at "main.c":31:7))
  sea.br ^err loc("main.c":33:3)
^err:
  %f = sea.const 0 : i1 loc("main.c":33:3)
  sea.assert %f loc("main.c":33:3)
  sea.halt loc("main.c":33:3)
}

// UNLOC: result: sat
// UNLOC: replay: assert-failed{{$}}
// UNLOC-NEXT: replay: assert-at: verify.c:12:3 inlined-at=main.c:31:7{{$}}
// UNLOC: replay: assert-op: {{.*}}assert-at-reach.mlir:[[#@LINE+16]]:3{{$}}
// UNLOC: replay: assert-path: main.c:30:7{{$}}
sea.program @unlocated {
  %x = sea.nondet "nd_u8" [0] : i8 loc("main.c":20:11)
  %c5 = sea.const 5 : i8 loc("main.c":30:11)
  %big = sea.icmp ugt, %x, %c5 : i8 loc("main.c":30:11)
  sea.cond_br %big, ^hit, ^miss loc("main.c":30:7)
^miss:
  sea.unreachable
^hit:
  %c9 = sea.const 9 : i8 loc(callsite("verify.c":12:15 at "main.c":31:7))
  %ok = sea.icmp ult, %x, %c9 : i8 loc(callsite("verify.c":12:15 at "main.c":31:7))
  sea.assume %ok loc(callsite("verify.c":12:3 at "main.c":31:7))
  sea.br ^err
^err:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
