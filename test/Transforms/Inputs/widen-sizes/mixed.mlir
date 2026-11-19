// Memories with different word sizes, read at offsets that are not multiples
// of the largest size (input of widen-memory-diff.test: the replay of a byte
// run must use each memory's own word size). a: 8-byte words; b: 4-byte
// words at 4 mod 8; c: 2-byte words at 2 mod 4; d: bytes.

!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @main {
  %a0 = sea.mem.nondet "a" [0] : !m
  %b0 = sea.mem.nondet "b" [1] : !m
  %c0 = sea.mem.nondet "c" [2] : !m
  %d0 = sea.mem.nondet "d" [3] : !m
  %n = sea.const 64 : i64
  %p = sea.alloca %n align 8 [4] : i64 -> !sea.ptr<64>
  %k2 = sea.const 2 : i64
  %k4 = sea.const 4 : i64
  %k8 = sea.const 8 : i64
  %k12 = sea.const 12 : i64
  %p2 = sea.ptr.add %p, %k2 : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %p, %k4 : !sea.ptr<64>, i64
  %p8 = sea.ptr.add %p, %k8 : !sea.ptr<64>, i64
  %p12 = sea.ptr.add %p, %k12 : !sea.ptr<64>, i64
  %a = sea.load %a0[%p8] : !m -> i64
  %b = sea.load %b0[%p4] : !m -> i32
  %b2 = sea.load %b0[%p12] : !m -> i32
  %c = sea.load %c0[%p2] : !m -> i16
  %d = sea.load %d0[%p4] : !m -> i8
  %at = sea.trunc %a : i64 to i32
  %s1 = sea.add %at, %b : i32
  %s2 = sea.add %s1, %b2 : i32
  %cz = sea.zext %c : i16 to i32
  %dz = sea.zext %d : i8 to i32
  %s3 = sea.add %s2, %cz : i32
  %s4 = sea.add %s3, %dz : i32
  %z = sea.const 12345 : i32
  %e = sea.icmp ne, %s4, %z : i32
  sea.assert %e
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
