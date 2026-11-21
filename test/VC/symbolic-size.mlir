// An allocation of non-constant size gets --max-symbolic-size units of room
// at placement. --symbolic-size=assume (default): the answer rests on
// assume(size <= room), a named assumption of the ledger. =assert: a premise
// assert, nothing assumed; a run that exceeds the room is a premise failure
// (exit 4), not a property failure. =refuse: an error.
//
// @free: the property (size <= 8192) holds only under the bound, so its
// unsat rests on the assumption.
//
// assume: the ledger line, with the allocations and the room
// RUN: sea-vc %s --program=free --lower 2>&1 | FileCheck %s --check-prefix=ASSUME
// ASSUME: Warning: {{.*}}[place.symbolic-size-assume]
// ASSUME: assume.symbolic-size-bound: allocs=1 room=4096
// ASSUME: result: unsat
//
// --max-symbolic-size sets the room (and the line)
// RUN: sea-vc %s --program=free --lower --max-symbolic-size=64 2>&1 | FileCheck %s --check-prefix=ROOM
// ROOM: assume.symbolic-size-bound: allocs=1 room=64
//
// assert, a size that can exceed the room: the premise assert fails in the
// replay, at the allocation, with the counterexample's size; exit 4
// RUN: not sea-vc %s --program=free --lower --symbolic-size=assert --brunch-stats > %t.free 2> %t.free.err
// RUN: FileCheck %s --check-prefix=FREE < %t.free
// RUN: FileCheck %s --check-prefix=FREE-ERR < %t.free.err
// FREE-NOT: assume.symbolic-size-bound
// FREE: result: sat
// FREE: nondet: site=1 callee=k width=64 value=[[K:[0-9]+]]
// FREE: replay: assert-failed
// FREE: replay: assert-op: a.c:3:7
// FREE: validate: checked={{[0-9]+}} mismatches=0
// FREE: premise.failed: symbolic-size-bound size=[[K]] room=4096 at=a.c:3:7
// FREE: BRUNCH_STAT status 4
// FREE-ERR: Error: sat, but the counterexample fails a premise assert (symbolic-size-bound), not the property{{.*}}[vc.premise-failed]
//
// the same as one jsonl record
// RUN: not sea-vc %s --program=free --lower --symbolic-size=assert --format=jsonl | FileCheck %s --check-prefix=JSONL
// JSONL: {"kind":"premise","status":"failed","name":"symbolic-size-bound","size":{{"?[0-9]+"?}},"room":4096,"at":"a.c:3:7",
//
// sea-prune-edges keeps only the failing side of the premise (the
// continuation blocks): `assume (xor c, 1); br` carries the mark, and the
// size and room still come from c
// RUN: not sea-vc %s --program=pruned --lower --symbolic-size=assert | FileCheck %s --check-prefix=PRUNED
// PRUNED: premise.failed: symbolic-size-bound size={{[0-9]+}} room=4096 at=a.c:3:7
//
// assert, the premise bounded by an earlier assume and a property that
// fails: the property's failure, no premise failure, exit 0
// RUN: sea-vc %s --program=prop --lower --symbolic-size=assert | FileCheck %s --check-prefix=PROP
// PROP-NOT: premise.failed
// PROP: replay: assert-op: a.c:9:3
// PROP-NOT: premise.failed
//
// the room bounds the premise too: with --max-symbolic-size the size found
// exceeds that room
// RUN: not sea-vc %s --program=free --lower --symbolic-size=assert --max-symbolic-size=64 | FileCheck %s --check-prefix=FREE64
// FREE64: premise.failed: symbolic-size-bound size={{[0-9]+}} room=64 at=a.c:3:7
//
// interpreter agreement (behaviour placement's assert mode had before
// sea-vc exposed it): on the placed program, a size above the room fails
// the premise assert at the allocation, one within it halts
// RUN: sea-opt %s --pass-pipeline='builtin.module(sea.program(sea-place-static{symbolic-size=assert}))' -mlir-print-debuginfo 2>/dev/null > %t.placed.mlir
// RUN: sea-run %t.placed.mlir --program=free --input=1=4097 | FileCheck %s --check-prefix=RUN-OVER
// RUN-OVER: status: assert-failed
// RUN-OVER: assert-op: a.c:3:7
// RUN: sea-run %t.placed.mlir --program=free --input=1=4096 | FileCheck %s --check-prefix=RUN-FITS
// RUN-FITS: status: halted
//
// assert, a size an earlier assume bounds below the room: unsat, nothing
// assumed
// RUN: sea-vc %s --program=bounded --lower --symbolic-size=assert 2>&1 | FileCheck %s --check-prefix=BOUNDED
// BOUNDED-NOT: assume.symbolic-size-bound
// BOUNDED: result: unsat
//
// refuse: an error
// RUN: not sea-vc %s --program=free --lower --symbolic-size=refuse 2>&1 | FileCheck %s --check-prefix=REFUSE
// REFUSE: Error: an allocation of non-constant size (symbolic-size=refuse) {{.*}}[place.symbolic-size-refused]
//
// the options need --lower; a mode is one of three
// RUN: not sea-vc %s --program=free --symbolic-size=assert 2>&1 | FileCheck %s --check-prefix=NOLOWER
// NOLOWER: --symbolic-size and --max-symbolic-size need --lower
// RUN: not sea-vc %s --program=free --lower --symbolic-size=maybe 2>&1 | FileCheck %s --check-prefix=BADMODE
// BADMODE: --symbolic-size must be assume, assert or refuse

!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @free {
  %m0 = sea.mem.nondet "m" [0] : !m
  %k = sea.nondet "k" [1] : i64
  %h = sea.malloc %k align 8 [2] : i64 -> !sea.ptr<64> loc("a.c":3:7)
  %c8192 = sea.const 8192 : i64
  %ok = sea.icmp ule, %k, %c8192 : i64
  sea.assert %ok loc("a.c":9:3)
  sea.halt
}
sea.program @bounded {
  %m0 = sea.mem.nondet "m" [0] : !m
  %k = sea.nondet "k" [1] : i64
  %c100 = sea.const 100 : i64
  %b = sea.icmp ule, %k, %c100 : i64
  sea.assume %b
  %h = sea.malloc %k align 8 [2] : i64 -> !sea.ptr<64> loc("a.c":3:7)
  %c5 = sea.const 5 : i8
  %m1 = sea.store %m0[%h], %c5 inplace : !m, i8
  %x = sea.load %m1[%h] : !m -> i8
  %ok = sea.icmp eq, %x, %c5 : i8
  sea.assert %ok loc("a.c":9:3)
  sea.halt
}

sea.program @pruned {
  %k = sea.nondet "k" [1] : i64
  %h = sea.malloc %k align 8 [2] : i64 -> !sea.ptr<64> loc("a.c":3:7)
  %f = sea.const 0 : i1
  sea.assume %f
  sea.halt
}
sea.program @prop {
  %m0 = sea.mem.nondet "m" [0] : !m
  %k = sea.nondet "k" [1] : i64
  %c100 = sea.const 100 : i64
  %b = sea.icmp ule, %k, %c100 : i64
  sea.assume %b
  %h = sea.malloc %k align 8 [2] : i64 -> !sea.ptr<64> loc("a.c":3:7)
  %x = sea.load %m0[%h] : !m -> i8
  %c5 = sea.const 5 : i8
  %ok = sea.icmp eq, %x, %c5 : i8
  sea.assert %ok loc("a.c":9:3)
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
