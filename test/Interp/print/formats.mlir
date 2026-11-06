// RUN: sea-run %s --input=0=-1,1=200,2=-2,3=0x8000000000000000 | FileCheck %s
// RUN: sea-run %s --input=0=-1,1=200,2=-2,3=0x8000000000000000 --format=jsonl | FileCheck %s --check-prefix=JSON
// An i1 under auto is a bool: a JSON boolean in jsonl.
// JSON: {"kind":"print","path":"bool",{{.*}}"type":"i1","fmt":"auto","value":true,

// Integer formats of sea.print (docs/design/Print.md): MLIR type suffixes;
// auto is cvlr's (decimal below 2^32, else hex plus a small negative).
sea.program @main {
  %m1 = sea.nondet "nondet_i32" [0] : i32 loc("m1")
  %b = sea.nondet "nondet_u8" [1] : i8 loc("b")
  %w = sea.nondet "nondet_u64" [2] : i64 loc("w")
  %big = sea.nondet "nondet_u64" [3] : i64 loc("big")
  %t = sea.const 1 : i1 loc("t")
  %c = sea.const 65 : i8 loc("c")
  %nl = sea.const 10 : i8 loc("nl")
  %two = sea.const 2 : i8 loc("two")
  %hi = sea.zext %w : i64 to i128 loc("wide")
  // CHECK: print: auto32 = 4294967295 : ui32
  sea.print "auto32" = "m1" : i32
  // CHECK: print: s32 = -1 : si32
  sea.print "s32" = "m1" : i32 {fmt = "s"}
  // CHECK: print: u32 = 4294967295 : ui32
  sea.print "u32" = "m1" : i32 {fmt = "u"}
  // CHECK: print: x32 = 0xffffffff : i32
  sea.print "x32" = "m1" : i32 {fmt = "x"}
  // CHECK: print: bits8 = 0b11001000 : i8
  sea.print "bits8" = "b" : i8 {fmt = "bits"}
  // CHECK: print: s8 = -56 : si8
  sea.print "s8" = "b" : i8 {fmt = "s"}
  // CHECK: print: auto64 = 0xfffffffffffffffe : i64 (-2)
  sea.print "auto64" = "w" : i64
  // the most negative i64 is not small: hex only
  // CHECK: print: min64 = 0x8000000000000000 : i64{{$}}
  sea.print "min64" = "big" : i64
  // CHECK: print: s64 = -9223372036854775808 : si64
  sea.print "s64" = "big" : i64 {fmt = "s"}
  // CHECK: print: wide = 18446744073709551614 : ui128
  sea.print "wide" = "wide" : i128 {fmt = "u"}
  // CHECK: print: bool = true
  sea.print "bool" = "t" : i1
  // CHECK: print: notbool = 0x2 : i8 (not a bool)
  sea.print "notbool" = "two" : i8 {fmt = "b"}
  // CHECK: print: char = 'A' : i8
  sea.print "char" = "c" : i8 {fmt = "c"}
  // CHECK: print: newline = '\x0a' : i8
  sea.print "newline" = "nl" : i8 {fmt = "c"}
  // by site, strong
  // CHECK: print: site = -2 : si64
  sea.print "site" = "nd.2" : i64 {fmt = "s"}
  // CHECK: print: strong = 200 : ui8
  sea.print "strong" %b : i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
