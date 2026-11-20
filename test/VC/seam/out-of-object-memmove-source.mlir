// A memmove's source range is read, not written: in the pairwise step of
// the static out-of-object report a read and a read never break A-SEP.
// A = [4096, 4104), B = [4112, 4120), C = [4128, 4136); the gap cell 4108
// is A + 12 and B - 4. @src: a load through A + 12 and a memmove whose
// source is B - 4: two reads, both land gap. @dst: the same memmove with
// B - 4 as its destination: a write and a read, both land shared (the
// assertion is trivial: only the report matters here).
// RUN: sea-vc %s --program=src --seam 2>&1 | FileCheck %s --check-prefix=SRC
// RUN: sea-vc %s --program=dst --seam 2>&1 | FileCheck %s --check-prefix=DST
// SRC-NOT: lands=shared
// SRC: seam.out-of-object: accesses=2 object=0 shared=0 unknown=0 null=0 gap=2
// DST: seam.out-of-object: accesses=2 object=0 shared=2 unknown=0 null=0 gap=0
!m = !sea.mem<!sea.addr<64>, i8>
sea.program @src {
  %A = sea.segment "heap.1" base 4096 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %B = sea.segment "heap.2" base 4112 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %C = sea.segment "heap.3" base 4128 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [2] : !m
  %c12 = sea.const 12 : i64
  %cm4 = sea.const -4 : i64
  %one = sea.const 1 : i64
  %p = sea.addr.add %A, %c12 : !sea.addr<64>, i64
  %q = sea.addr.add %B, %cm4 : !sea.addr<64>, i64
  %x = sea.load %m0[%p] : !m -> i8
  %m1 = sea.memmove %m0[%C], %m0[%q], %one inplace : !m
  %y = sea.load %m1[%C] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}
sea.program @dst {
  %A = sea.segment "heap.1" base 4096 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %B = sea.segment "heap.2" base 4112 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %C = sea.segment "heap.3" base 4128 : i64 size 8 : i64 align 8 kind "heap" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [2] : !m
  %c12 = sea.const 12 : i64
  %cm4 = sea.const -4 : i64
  %one = sea.const 1 : i64
  %p = sea.addr.add %A, %c12 : !sea.addr<64>, i64
  %q = sea.addr.add %B, %cm4 : !sea.addr<64>, i64
  %m1 = sea.memmove %m0[%q], %m0[%C], %one inplace : !m
  %x = sea.load %m1[%p] : !m -> i8
  %y = sea.load %m1[%C] : !m -> i8
  %ok = sea.icmp eq, %x, %x : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
