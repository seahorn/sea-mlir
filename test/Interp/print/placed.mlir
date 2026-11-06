// RUN: sea-run %s | FileCheck %s

// A hand-placed program: A at 16 (16 bytes), B right after at 32. An
// address without provenance (addr.from_int) prints as a number with a
// `~` hint, never as a pointer into an object; bits of an address keep
// their object through to_bits / extract / concat / from_bits and memory.
!m = !sea.mem<!sea.addr<32>, i8>
sea.program @main {
  %A = sea.segment "heap.1" base 16 : i32 size 16 : i32 align 8 kind "heap" label "A" : !sea.addr<32> loc("A")
  %B = sea.segment "heap.2" base 32 : i32 size 8 : i32 align 8 kind "heap" : !sea.addr<32> loc("B")
  %c16 = sea.const 16 : i32
  %c36 = sea.const 36 : i32
  %c64 = sea.const 64 : i32
  %end = sea.addr.add %A, %c16 : !sea.addr<32>, i32 loc("end")
  %raw = sea.addr.from_int %c36 : i32 to !sea.addr<32> loc("raw")
  %far = sea.addr.from_int %c64 : i32 to !sea.addr<32> loc("far")
  %bits = sea.addr.to_bits %end : !sea.addr<32> to i32
  %lo = sea.extract %bits[0] : i32 -> i16
  %hi = sea.extract %bits[16] : i32 -> i16
  %re = sea.concat %hi, %lo : i16, i16 -> i32
  %back = sea.addr.from_bits %re : i32 to !sea.addr<32> loc("back")
  %swap = sea.concat %lo, %hi : i16, i16 -> i32
  %wrong = sea.addr.from_bits %swap : i32 to !sea.addr<32> loc("wrong")
  %m0 = sea.mem.nondet "heap" [3] : !m loc("mem")
  %m1 = sea.store %m0[%B], %end inplace : !m, !sea.addr<32>
  // CHECK: print.object: heap.1 kind=heap size=16 label=A {{.*}}base=0x10
  // CHECK-NEXT: print: end = &heap.1[16] end
  sea.print "end" = "end" : !sea.addr<32>
  // CHECK-NEXT: print.object: heap.2 kind=heap size=8 {{.*}}base=0x20
  // CHECK-NEXT: print: B = &heap.2[0]
  sea.print "B" = "B" : !sea.addr<32>
  // CHECK-NEXT: print: raw = 0x24 ~heap.2[4]
  sea.print "raw" = "raw" : !sea.addr<32>
  // CHECK-NEXT: print: far = 0x40{{$}}
  sea.print "far" = "far" : !sea.addr<32>
  // CHECK-NEXT: print: back = &heap.1[16] end
  sea.print "back" = "back" : !sea.addr<32>
  // CHECK-NEXT: print: wrong = 0x200000{{$}}
  sea.print "wrong" = "wrong" : !sea.addr<32>
  // CHECK-NEXT: print: stored = &heap.1[16] end
  sea.print.mem "stored" = "mem"["B"] : !sea.addr<32>
  // CHECK-NEXT: print: bytes = heap.2[0..4]
  // CHECK-NEXT: print: bytes +00: 20 00 00 00 |
  sea.print.mem "bytes" = "mem"["B"] : i8 {fmt = "hexdump", count = 4}
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
