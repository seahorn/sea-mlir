// RUN: not sea-vc %s --program=direct 2>&1 | FileCheck %s
// RUN: bash -c 'sea-vc %s --program=direct > /dev/null 2>&1; test $? -eq 2'
// RUN: not sea-vc %s --program=direct --format=jsonl 2>/dev/null | FileCheck %s --check-prefix=JSONL
// RUN: not sea-vc %s --program=over 2>&1 | FileCheck %s --check-prefix=OVER
// RUN: sea-vc %s --program=unclobbered 2>&1 | FileCheck %s --check-prefix=OK
// RUN: sea-vc %s --program=unclobbered --lower --replay-original 2>&1 | FileCheck %s --check-prefix=ORIG

// A memmove whose source is an earlier memory version (Memory.md: allowed,
// on Mem-SSA's claim that the source range was not clobbered since). A
// memmove's result is a memory value, which validation cannot confirm, so a
// memmove that a mismatched value depends on is named with "may" wording
// (its claim is not checked per cell), and when only memmoves are named,
// vc.replay-mismatch leaves a VC bug open.
//
// @direct: the store clobbers the source cell; the load of the copy (itself
// of the current version) mismatches; only the memmove is named.
// CHECK: mismatch:
// CHECK: replay: no-clobber-violated: {{.*}}no-clobber-violated-memmove.mlir:[[#@LINE+32]]:9 (a memmove from an earlier memory version that a mismatched value depends on: it may have copied cells overwritten since; its no-clobber claim is not checked per cell)
// CHECK-NOT: replay: no-clobber-violated:
// CHECK: a VC or interpreter bug, or the Mem-SSA no-clobber claim of 1 memmove(s){{.*}}[vc.replay-mismatch]
// CHECK: result: unknown
// JSONL: {"kind":"no-clobber-violated","loc":"{{.*}}no-clobber-violated-memmove.mlir:[[#@LINE+28]]:9","text":"replay: no-clobber-violated:
//
// @over: an innocent memmove from an earlier version (nothing overwrote its
// source range) upstream of a load of an earlier version whose cell was
// clobbered. The load is named as the cause; the memmove gets the "may"
// text; the cause is the claim (a load is named).
// OVER: replay: no-clobber-violated: {{.*}}no-clobber-violated-memmove.mlir:[[#@LINE+43]]:8 (a load of an earlier memory version read a cell overwritten since; the program's Mem-SSA no-clobber claim fails here)
// OVER: replay: no-clobber-violated: {{.*}}no-clobber-violated-memmove.mlir:[[#@LINE+40]]:9 (a memmove from an earlier memory version that a mismatched value depends on: it may have copied
// OVER: the cause is the program's Mem-SSA no-clobber claim, not the VC: 1 load(s) of an earlier memory version{{.*}}[vc.replay-mismatch]
//
// @unclobbered: the store writes another cell, the claim holds, the replay
// is clean and counts the memmove (no per-op warning); a memmove of length
// 0 from an earlier version reads nothing and is not counted.
// OK: validate: checked={{[0-9]+}} mismatches=0
// OK: interp.earlier-version-memmoves: 1
// OK: result: sat
// OK-NOT: no-clobber-violated
// OK-NOT: interp.memmove-earlier-version
// ORIG: replay.original: {{.*}}match=yes{{.*}} earlier-version-memmoves=1
!m = !sea.mem<!sea.addr<8>, i8>
sea.program @direct {
  %m0 = sea.mem.nondet "r" [0] : !m
  %ai = sea.nondet "a" [1] : i8
  %a = sea.addr.from_int %ai : i8 to !sea.addr<8>
  %one = sea.const 1 : i8
  %b = sea.addr.add %a, %one : !sea.addr<8>, i8
  %seven = sea.const 7 : i8
  %m1 = sea.store %m0[%a], %seven inplace : !m, i8
  %m2 = sea.memmove %m1[%b], %m0[%a], %one inplace : !m
  %x = sea.load %m2[%b] : !m -> i8
  %ok = sea.icmp eq, %x, %seven : i8
  sea.assert %ok
  sea.halt
}
sea.program @over {
  %m0 = sea.mem.nondet "r" [0] : !m
  %ai = sea.nondet "a" [1] : i8
  %a = sea.addr.from_int %ai : i8 to !sea.addr<8>
  %one = sea.const 1 : i8
  %two = sea.const 2 : i8
  %five = sea.const 5 : i8
  %b = sea.addr.add %a, %one : !sea.addr<8>, i8
  %c = sea.addr.add %a, %two : !sea.addr<8>, i8
  %d = sea.addr.add %a, %five : !sea.addr<8>, i8
  %seven = sea.const 7 : i8
  %m1 = sea.memmove %m0[%c], %m0[%d], %one inplace : !m
  %m2 = sea.store %m1[%b], %seven inplace : !m, i8
  %m3 = sea.memmove %m2[%d], %m0[%a], %one inplace : !m
  %m4 = sea.store %m3[%a], %seven inplace : !m, i8
  %x = sea.load %m3[%a] : !m -> i8
  %ok = sea.icmp eq, %x, %seven : i8
  sea.assert %ok
  sea.halt
}
sea.program @unclobbered {
  %m0 = sea.mem.nondet "r" [0] : !m
  %ai = sea.nondet "a" [1] : i8
  %a = sea.addr.from_int %ai : i8 to !sea.addr<8>
  %zero = sea.const 0 : i8
  %one = sea.const 1 : i8
  %two = sea.const 2 : i8
  %b = sea.addr.add %a, %one : !sea.addr<8>, i8
  %c = sea.addr.add %a, %two : !sea.addr<8>, i8
  %seven = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %seven inplace : !m, i8
  %m2 = sea.memmove %m1[%c], %m0[%a], %one inplace : !m
  %m3 = sea.memmove %m2[%a], %m0[%b], %zero inplace : !m
  %x = sea.load %m3[%c] : !m -> i8
  %ok = sea.icmp eq, %x, %seven : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
