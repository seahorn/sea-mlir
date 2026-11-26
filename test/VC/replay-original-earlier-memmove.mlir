// RUN: sea-vc %s --lower --place-slots=none --replay-original 2>&1 | FileCheck %s

// --replay-original on a program whose behavior depends on placement (one
// past the end of a is b, as compare.mlir in replay-original.test) that
// also copies from an earlier memory version: the original run does not
// match, and since the in-place interpreter cannot check the memmove's
// no-clobber claim, the answer is undetermined (a warning, exit unchanged),
// not a lowering mismatch (exit 6).
// CHECK: replay.original: status=halted at=- match=undetermined {{.*}} earlier-version-memmoves=1
// CHECK: the original program reads an earlier memory version (memmove source or is_modified{{.*}}[vc.replay-original-undetermined]
// CHECK: result: sat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ope {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %one = sea.const 1 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %c = sea.malloc %c16 align 16 [3] : i64 -> !sea.ptr<64>
  %v7 = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %v7 inplace : !m, i8
  %m2 = sea.memmove %m1[%a], %m0[%c], %one inplace : !m
  %e = sea.ptr.add %a, %c16 : !sea.ptr<64>, i64
  %eq = sea.ptr.cmp eq, %e, %b : !sea.ptr<64>
  %s = sea.select %eq, %b, %c : !sea.ptr<64>
  %x = sea.load %m2[%s] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
