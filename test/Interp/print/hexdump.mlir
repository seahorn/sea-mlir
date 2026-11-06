// RUN: sea-run %s | FileCheck %s
// RUN: sea-run %s --missing=zero | FileCheck %s --check-prefix=ZERO

// Hex dumps (SeaHorn's HexDump layout): 16 bytes a row with the ascii
// column, a repeated row squeezed to `*`, bytes no source gives `??`
// (sea-run without an input: a print never fails the run nor draws one).
!m = !sea.mem<!sea.addr<32>, i8>
sea.program @main {
  %m0 = sea.mem.nondet "mem" [1] : !m loc("mem")
  %a = sea.const 0 : i32
  %base = sea.addr.from_int %a : i32 to !sea.addr<32>
  %c0 = sea.const 0 : i8
  %n = sea.const 64 : i32
  %m1 = sea.memset %m0[%base], %c0, %n inplace : !m
  %at = sea.const 64 : i32
  %p = sea.addr.from_int %at : i32 to !sea.addr<32> loc("p")
  %v = sea.const 0x6f6c6c6568 : i40
  %m2 = sea.store %m1[%p], %v inplace : !m, i40
  %z = sea.addr.from_int %a : i32 to !sea.addr<32> loc("z")
  // CHECK: print: zeros = [0x0..0x50]
  // CHECK-NEXT: print: zeros +00: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 |................|
  // CHECK-NEXT: print: zeros *
  // CHECK-NEXT: print: zeros +40: 68 65 6c 6c 6f ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? |hello???????????|
  sea.print.mem "zeros" = "mem"["z"] : i8 {fmt = "hexdump", count = 80}
  // CHECK: print: tail = [0x40..0x47]
  // CHECK-NEXT: print: tail +00: 68 65 6c 6c 6f ?? ??                            |hello??|
  // ZERO: print: tail +00: 68 65 6c 6c 6f 00 00                            |hello..|
  sea.print.mem "tail" = "mem"["p"] : i8 {fmt = "hexdump", count = 7}
  // typed reads, little-endian; an element with an unreadable byte is `?`
  // CHECK: print: w = 1819043176 : ui32
  sea.print.mem "w" = "mem"["p"] : i32 {fmt = "u"}
  // CHECK: print: w2[0] = 0x6c6c6568 : i32
  // CHECK-NEXT: print: w2[1] = ?
  sea.print.mem "w2" = "mem"["p"] : i32 {fmt = "x", count = 2}
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
