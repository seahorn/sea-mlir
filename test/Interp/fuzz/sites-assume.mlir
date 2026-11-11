// --fuzz-sites lists every op that can block a run, before any run: one
// fuzz.assume line per sea.assume (with its condition over input sites)
// and sea.unreachable (no condition), in program order; the id is the
// position in that order. Then one fuzz.range line per value site with a
// range. The count line comes last.
// RUN: sea-run %s --fuzz-sites | FileCheck %s
// RUN: sea-run %s --fuzz-sites --format=jsonl > %t.jsonl
// RUN: FileCheck %s --check-prefix=JSONL < %t.jsonl
// RUN: sea-run %s --fuzz-sites > %t.txt
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl --require stat:op,at

// CHECK: fuzz.site: 6 callee=nd_size_t
// CHECK-NEXT: fuzz.site: 7 callee=nd_size_t
// CHECK-NEXT: fuzz.assume: 0 op=sea.assume at={{.*}}sites-assume.mlir:[[@LINE+18]]:3 sites=6,7 cond=(ule s6 s7)
// CHECK-NEXT: fuzz.assume: 1 op=sea.assume at={{.*}}sites-assume.mlir:[[@LINE+19]]:3 sites=7 cond=(ult s7 11)
// CHECK-NEXT: fuzz.assume: 2 op=sea.unreachable at={{.*}}sites-assume.mlir:[[@LINE+25]]:3{{$}}
// CHECK-NEXT: fuzz.assume: 3 op=sea.assume at={{.*}}sites-assume.mlir:[[@LINE+26]]:3 sites=- cond=?
// the ranges of the generator (--fuzz-ranges), read off assumes 0 and 1
// CHECK-NEXT: fuzz.range: 6 width=64 lo=0 span=- bounds=s7 assumes=0{{$}}
// CHECK-NEXT: fuzz.range: 7 width=64 lo=0 span=10 bounds=- assumes=1{{$}}
// CHECK-NEXT: fuzz.sites: 2 value=2 memory=0 assumes=3 unreachables=1{{$}}
// CHECK-NOT: fuzz.
// CHECK-NOT: status:
// JSONL: {"kind":"stat","key":"fuzz.assume","value":0,"op":"sea.assume","at":"{{.*}}","sites":"6,7","cond":"(ule s6 s7)","text":"fuzz.assume: 0 {{.*}}"}
// JSONL: {"kind":"stat","key":"fuzz.assume","value":2,"op":"sea.unreachable","at":"{{.*}}","text":"fuzz.assume: 2 {{.*}}"}
// JSONL: {"kind":"stat","key":"fuzz.sites",{{.*}}"assumes":3,"unreachables":1,
sea.program @main {
  %len = sea.nondet "nd_size_t" [6] : i64
  %cap = sea.nondet "nd_size_t" [7] : i64
  %c11 = sea.const 11 : i64
  %le = sea.icmp ule, %len, %cap : i64
  sea.assume %le
  %lt = sea.icmp ult, %cap, %c11 : i64
  sea.assume %lt
  %z = sea.const 0 : i64
  %isz = sea.icmp eq, %len, %z : i64
  sea.cond_br %isz, ^zero, ^merge(%le : i1)
^zero:
  sea.cond_br %lt, ^dead, ^merge(%lt : i1)
^dead:
  sea.unreachable
^merge(%m : i1):
  sea.assume %m
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
