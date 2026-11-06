// --ptr-companion=check: the property is the A-PTRINT guard of every split
// pointer load, not the program's assertions (OpSem drops them). A sat
// answer is confirmed when the replay sees a guard fail: the pointer last
// stored at the load's address differs from the loaded bits
// (seam.ptrint-violated), or no pointer was stored there
// (seam.ptrint-unstored: A-PTRINT names no pointer). The program's
// assertion may hold on that run (replay: halted): exit 0, as for a sat
// confirmed by replay. A check-mode sat whose replay shows no failing guard
// stays the error vc.replay-no-failure (exit 2).
//  @unstored: one CFG arm stores the pointer's bytes, the other two
//    integers; the integer arm reads a cell no pointer was stored to, and
//    the assertion (the loaded bits are the expected ones) holds.
//  @overwritten: the pointer's bytes, then an integer over its low byte;
//    the reloaded bits are not the stored pointer's, and the assertion
//    (to_bits of the load is the bits) holds.
//  @fails: as @unstored, but the assertion fails on the integer arm: the
//    replay fails it, and the guard is reported too.
//  @both: both arms store a pointer: unsat, A-PTRINT holds.
// RUN: sea-vc %s --program=unstored --seam --seam-inb=0 --ptr-companion=check --seam-check 2>&1 | FileCheck %s --check-prefix=UNST --implicit-check-not=Error
// RUN: sea-vc %s --program=overwritten --seam --ptr-companion=check 2>&1 | FileCheck %s --check-prefix=OVER --implicit-check-not=Error
// RUN: sea-vc %s --program=fails --seam --seam-inb=0 --ptr-companion=check 2>&1 | FileCheck %s --check-prefix=FAILS --implicit-check-not=Error
// RUN: sea-vc %s --program=both --seam --seam-inb=0 --ptr-companion=check 2>&1 | FileCheck %s --check-prefix=BOTH --implicit-check-not=Error
// RUN: sea-vc %s --program=overwritten --seam --ptr-companion=check --format=jsonl 2>&1 | FileCheck %s --check-prefix=JSON
// RUN: sea-vc %s --program=overwritten --seam --ptr-companion=exact 2>&1 | FileCheck %s --check-prefix=EXACT --implicit-check-not=seam.ptrint-
// UNST: seam.ptrint: checked loads=1
// UNST: result: sat
// UNST: seam.ptrint: not discharged
// UNST: replay: halted
// UNST: validate: checked={{[0-9]+}} mismatches=0
// UNST: seam.ptrint-unstored: {{.*}}ptr-companion-check-replay.mlir:79:8 addr=256 obj=&slot[0] bits=513{{$}}
// UNST: seam.ptrint-replay: loads=1 holds=0 violated=0 unstored=1
// UNST: seam-check: {{.*}} invalid=0
// OVER: result: sat
// OVER: seam.ptrint: not discharged
// OVER: replay: halted
// OVER: validate: checked={{[0-9]+}} mismatches=0
// OVER: seam.ptrint-violated: {{.*}}ptr-companion-check-replay.mlir:104:8 addr=256 obj=&slot[0] bits=519 stored=512
// OVER: seam.ptrint-replay: loads=1 holds=0 violated=1 unstored=0
// FAILS: result: sat
// FAILS: replay: assert-failed
// FAILS: validate: checked={{[0-9]+}} mismatches=0
// FAILS: seam.ptrint-unstored: {{.*}} addr=256 obj=&slot[0] bits=513{{$}}
// FAILS: seam.ptrint-replay: loads=1 holds=0 violated=0 unstored=1
// BOTH: result: unsat
// BOTH: seam.ptrint: holds
// JSON: {"kind":"ptrint","status":"violated",{{.*}}"addr":256,"obj":"slot","off":0,"bits":519,"stored":512,
// JSON: {"kind":"ptrint-replay","loads":1,"holds":0,"violated":1,"unstored":0,
// EXACT: result: unsat

!m = !sea.mem<!sea.addr<16>, i8>
sea.program @unstored {
  %c = sea.nondet "choose" [0] : i8
  %zero8 = sea.const 0 : i8
  %take = sea.icmp eq, %c, %zero8 : i8
  %slot = sea.segment "slot" base 256 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %p = sea.segment "p" base 512 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %bits = sea.addr.to_bits %p : !sea.addr<16> to i16
  %lo = sea.extract %bits[0] : i16 -> i8
  %hi = sea.extract %bits[8] : i16 -> i8
  %one = sea.const 1 : i16
  %one8 = sea.const 1 : i8
  %two8 = sea.const 2 : i8
  %other = sea.const 513 : i16
  %slot1 = sea.addr.add %slot, %one : !sea.addr<16>, i16
  %m0 = sea.mem.const %zero8 : !m
  sea.cond_br %take, ^pointer, ^integer
^pointer:
  %ma = sea.store %m0[%slot], %lo inplace : !m, i8
  %mb = sea.store %ma[%slot1], %hi inplace : !m, i8
  sea.br ^join(%mb : !m)
^integer:
  %mc = sea.store %m0[%slot], %one8 inplace : !m, i8
  %md = sea.store %mc[%slot1], %two8 inplace : !m, i8
  sea.br ^join(%md : !m)
^join(%m : !m):
  %l = sea.load %m[%slot] : !m -> i8
  %h = sea.load %m[%slot1] : !m -> i8
  %word = sea.concat %h, %l : i8, i8 -> i16
  %q = sea.addr.from_bits %word : i16 to !sea.addr<16>
  %qb = sea.addr.to_bits %q : !sea.addr<16> to i16
  %expected = sea.select %take, %bits, %other : i16
  %ok = sea.icmp eq, %qb, %expected : i16
  sea.assert %ok
  sea.halt
}

sea.program @overwritten {
  %zero8 = sea.const 0 : i8
  %seven8 = sea.const 7 : i8
  %slot = sea.segment "slot" base 256 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %p = sea.segment "p" base 512 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %bits = sea.addr.to_bits %p : !sea.addr<16> to i16
  %lo = sea.extract %bits[0] : i16 -> i8
  %hi = sea.extract %bits[8] : i16 -> i8
  %one = sea.const 1 : i16
  %slot1 = sea.addr.add %slot, %one : !sea.addr<16>, i16
  %m0 = sea.mem.const %zero8 : !m
  %m1 = sea.store %m0[%slot], %lo inplace : !m, i8
  %m2 = sea.store %m1[%slot1], %hi inplace : !m, i8
  %m3 = sea.store %m2[%slot], %seven8 inplace : !m, i8
  %l = sea.load %m3[%slot] : !m -> i8
  %h = sea.load %m3[%slot1] : !m -> i8
  %word = sea.concat %h, %l : i8, i8 -> i16
  %q = sea.addr.from_bits %word : i16 to !sea.addr<16>
  %qb = sea.addr.to_bits %q : !sea.addr<16> to i16
  %ok = sea.icmp eq, %qb, %word : i16
  sea.assert %ok
  sea.halt
}

sea.program @fails {
  %c = sea.nondet "choose" [0] : i8
  %zero8 = sea.const 0 : i8
  %take = sea.icmp eq, %c, %zero8 : i8
  %slot = sea.segment "slot" base 256 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %p = sea.segment "p" base 512 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %bits = sea.addr.to_bits %p : !sea.addr<16> to i16
  %lo = sea.extract %bits[0] : i16 -> i8
  %hi = sea.extract %bits[8] : i16 -> i8
  %one = sea.const 1 : i16
  %one8 = sea.const 1 : i8
  %two8 = sea.const 2 : i8
  %slot1 = sea.addr.add %slot, %one : !sea.addr<16>, i16
  %m0 = sea.mem.const %zero8 : !m
  sea.cond_br %take, ^pointer, ^integer
^pointer:
  %ma = sea.store %m0[%slot], %lo inplace : !m, i8
  %mb = sea.store %ma[%slot1], %hi inplace : !m, i8
  sea.br ^join(%mb : !m)
^integer:
  %mc = sea.store %m0[%slot], %one8 inplace : !m, i8
  %md = sea.store %mc[%slot1], %two8 inplace : !m, i8
  sea.br ^join(%md : !m)
^join(%m : !m):
  %l = sea.load %m[%slot] : !m -> i8
  %h = sea.load %m[%slot1] : !m -> i8
  %word = sea.concat %h, %l : i8, i8 -> i16
  %q = sea.addr.from_bits %word : i16 to !sea.addr<16>
  %qb = sea.addr.to_bits %q : !sea.addr<16> to i16
  %ok = sea.icmp eq, %qb, %bits : i16
  sea.assert %ok
  sea.halt
}

sea.program @both {
  %c = sea.nondet "choose" [0] : i8
  %zero8 = sea.const 0 : i8
  %take = sea.icmp eq, %c, %zero8 : i8
  %slot = sea.segment "slot" base 256 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %p = sea.segment "p" base 512 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %r = sea.segment "r" base 768 : i16 size 2 : i16 align 2 kind "stack" : !sea.addr<16>
  %pick = sea.select %take, %p, %r : !sea.addr<16>
  %bits = sea.addr.to_bits %pick : !sea.addr<16> to i16
  %lo = sea.extract %bits[0] : i16 -> i8
  %hi = sea.extract %bits[8] : i16 -> i8
  %one = sea.const 1 : i16
  %slot1 = sea.addr.add %slot, %one : !sea.addr<16>, i16
  %m0 = sea.mem.const %zero8 : !m
  %m1 = sea.store %m0[%slot], %lo inplace : !m, i8
  %m2 = sea.store %m1[%slot1], %hi inplace : !m, i8
  %l = sea.load %m2[%slot] : !m -> i8
  %h = sea.load %m2[%slot1] : !m -> i8
  %word = sea.concat %h, %l : i8, i8 -> i16
  %q = sea.addr.from_bits %word : i16 to !sea.addr<16>
  %qb = sea.addr.to_bits %q : !sea.addr<16> to i16
  %ok = sea.icmp eq, %qb, %bits : i16
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
