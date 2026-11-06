// Name-only locations (loc("name"), no file) in the locations of a failing
// run: assert-at, assert-op and print at= show the op's position in this
// file and the name (`<pos> name=<name>`; jsonl: `at` / `at_name`, `op` /
// `op_name`). With a file location inside, the name follows it.
// RUN: sea-vc %s --program=named --lower | FileCheck %s --check-prefix=NAMED
// RUN: sea-vc %s --program=named --lower --format=jsonl \
// RUN:   | FileCheck %s --check-prefix=NJSON
// RUN: sea-run %s --program=named --input=0=7 --print-locs \
// RUN:   | FileCheck %s --check-prefix=INTERP
// RUN: sea-run %s --program=fallback --input=0=1 | FileCheck %s --check-prefix=FALLBACK
// RUN: sea-run %s --program=fallback --input=0=1 --format=jsonl \
// RUN:   | FileCheck %s --check-prefix=FJSON
// RUN: sea-vc %s --program=named --lower > %t.txt
// RUN: sea-vc %s --program=named --lower --format=jsonl > %t.jsonl
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl

// NAMED: replay: assert-failed{{$}}
// NAMED-NEXT: replay: assert-at: {{.*}}name-locations.mlir:[[#COND:@LINE+19]]:9 name=check.ne7{{$}}
// NAMED: replay: assert-op: {{.*}}name-locations.mlir:[[#OP:@LINE+21]]:3 name=claim.ne7{{$}}
// NJSON: "kind":"replay"
// NJSON-SAME: "at":"{{.*}}name-locations.mlir:[[#COND:@LINE+16]]:9","at_name":"check.ne7"
// NJSON-SAME: "op":"{{.*}}name-locations.mlir:[[#OP:@LINE+18]]:3","op_name":"claim.ne7"
// INTERP: print.object: stack.1 kind=stack size=8 at={{.*}}name-locations.mlir:[[#BUF:@LINE+11]]:8 at_name=buf{{$}}
// INTERP: print: x = 7 : {{u?}}i8 at={{.*}}name-locations.mlir:[[#PRINT:@LINE+15]]:3 at_name=show.x{{$}}
// INTERP: status: assert-failed
// INTERP-NEXT: at: loc("claim.ne7"("{{.*}}name-locations.mlir":[[#OP:@LINE+14]]:3)){{$}}
// INTERP-NEXT: assert-at: {{.*}}name-locations.mlir:[[#COND:@LINE+10]]:9 name=check.ne7{{$}}
// INTERP-NEXT: assert-op: {{.*}}name-locations.mlir:[[#OP]]:3 name=claim.ne7{{$}}
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @named {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c7 = sea.const 7 : i8
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64> loc("buf")
  %m0 = sea.mem.nondet "region" [2] : !m
  %m1 = sea.store %m0[%p], %x inplace : !m, i8
  %ok = sea.icmp ne, %x, %c7 : i8 loc("check.ne7")
  sea.print "p" = "buf" : !sea.ptr<64>
  sea.print "x" = "nd.0" : i8 loc("show.x")
  sea.assert %ok loc("claim.ne7")
  sea.halt
}

// `assert false` reached by a branch: the last located op before it, a
// name over a position of this file only.
// FALLBACK: status: assert-failed
// FALLBACK: assert-at: {{.*}}name-locations.mlir:[[#BR:@LINE+9]]:3 name=check.one{{$}}
// FALLBACK: assert-op: {{.*}}name-locations.mlir:[[#BR+5]]:3{{$}}
// FJSON: "kind":"status"
// FJSON-SAME: "at":"{{.*}}name-locations.mlir:[[#BR:@LINE+6]]:3","at_name":"check.one"
// FJSON-NOT: op_name
sea.program @fallback {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c1 = sea.const 1 : i8
  %ne = sea.icmp ne, %x, %c1 : i8
  sea.cond_br %ne, ^ok, ^err loc("check.one")
^ok:
  sea.halt
^err:
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
