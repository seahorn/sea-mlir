// RUN: sea-run %s --program=aligned --missing=zero | FileCheck %s --check-prefix=AL
// RUN: not sea-run %s --program=misaligned --missing=zero 2>&1 | FileCheck %s --check-prefix=MIS
// RUN: not sea-run %s --program=object_align --missing=zero 2>&1 | FileCheck %s --check-prefix=OBJ
// RUN: not sea-run %s --program=partial --missing=zero 2>&1 | FileCheck %s --check-prefix=PART
// RUN: not sea-run %s --program=out_of_bounds --missing=zero 2>&1 | FileCheck %s --check-prefix=OOB
// RUN: sea-run %s --program=unaligned --missing=zero | FileCheck %s --check-prefix=UN
// RUN: not sea-run %s --program=overlap --missing=zero 2>&1 | FileCheck %s --check-prefix=OVL
// RUN: sea-opt %s | sea-opt | FileCheck %s --check-prefix=RT
// RUN: sea-opt %s --sea-place-static --sea-split-wide-access | sea-run - --program=aligned --missing=zero | FileCheck %s --check-prefix=AL
// RUN: sea-vc %s --program=aligned --lower 2>&1 | FileCheck %s --check-prefix=VC
// RUN: sea-vc %s --program=unaligned --lower 2>&1 | FileCheck %s --check-prefix=VCU

// Word layouts: k-byte cells; addresses, offsets and lengths stay in bytes.

!w = !sea.mem<!sea.ptr<64>, i32, aligned>
!u = !sea.mem<!sea.ptr<64>, i32, unaligned>

// RT: !sea.mem<!sea.ptr<64>, i32, aligned>
// AL: status: halted
// The VC encodes words as array cells keyed by their first byte.
// VC: result: unsat
// VCU: [vc.unaligned-layout]
// VCU: result: unsat
sea.program @aligned {
  %m0 = sea.mem.nondet "m" [0] : !w
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %four = sea.const 4 : i64
  %q = sea.ptr.add %p, %four : !sea.ptr<64>, i64
  // a 64-bit store is two cells, at p+4 and p+8 (little-endian)
  %v = sea.const 0x1122334455667788 : i64
  %m1 = sea.store %m0[%q], %v inplace : !w, i64
  %eight = sea.const 8 : i64
  %r = sea.ptr.add %p, %eight : !sea.ptr<64>, i64
  %hi = sea.load %m1[%r] : !w -> i32
  %ehi = sea.const 0x11223344 : i32
  %a1 = sea.icmp eq, %hi, %ehi : i32
  // memset's value is a byte, repeated in every cell; length in bytes
  %b = sea.const 0xab : i8
  %m2 = sea.memset %m1[%p], %b, %four inplace : !w
  %w0 = sea.load %m2[%p] : !w -> i32
  %ew0 = sea.const 0xabababab : i32
  %a2 = sea.icmp eq, %w0, %ew0 : i32
  // memmove copies whole cells: p+8 (bytes 8..11) to p+12
  %twelve = sea.const 12 : i64
  %s = sea.ptr.add %p, %twelve : !sea.ptr<64>, i64
  %m3 = sea.memmove %m2[%s], %m2[%r], %four inplace : !w
  %w3 = sea.load %m3[%s] : !w -> i32
  %a3 = sea.icmp eq, %w3, %ehi : i32
  %c1 = sea.and %a1, %a2 : i1
  %ok = sea.and %c1, %a3 : i1
  sea.assert %ok
  sea.halt
}

// MIS: message: aligned 4-byte word layout: access at stack.1+2 is not word-aligned
sea.program @misaligned {
  %m0 = sea.mem.nondet "m" [0] : !w
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %two = sea.const 2 : i64
  %q = sea.ptr.add %p, %two : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] : !w -> i32
  sea.halt
}

// OBJ: message: aligned 4-byte word layout: stack.1 is aligned to 2 only
sea.program @object_align {
  %m0 = sea.mem.nondet "m" [0] : !w
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 2 [1] : i64 -> !sea.ptr<64>
  %x = sea.load %m0[%p] : !w -> i32
  sea.halt
}

// PART: message: memset of 6 bytes is not whole 4-byte cells
sea.program @partial {
  %m0 = sea.mem.nondet "m" [0] : !w
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %six = sea.const 6 : i64
  %b = sea.const 0 : i8
  %m1 = sea.memset %m0[%p], %b, %six inplace : !w
  sea.halt
}

// The whole cell must lie in the object.
// OOB: message: memory access out of bounds
sea.program @out_of_bounds {
  %m0 = sea.mem.nondet "m" [0] : !u
  %n = sea.const 6 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %four = sea.const 4 : i64
  %q = sea.ptr.add %p, %four : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] : !u -> i32
  sea.halt
}

// Unaligned: a cell at any byte offset, as long as accesses match.
// UN: status: halted
sea.program @unaligned {
  %m0 = sea.mem.nondet "m" [0] : !u
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %three = sea.const 3 : i64
  %q = sea.ptr.add %p, %three : !sea.ptr<64>, i64
  %v = sea.const 77 : i32
  %m1 = sea.store %m0[%q], %v inplace : !u, i32
  %x = sea.load %m1[%q] : !u -> i32
  %ok = sea.icmp eq, %x, %v : i32
  sea.assert %ok
  sea.halt
}

// OVL: message: unaligned word layout: the access at stack.1+1 overlaps the 4-byte cell at 3 (accesses do not match)
sea.program @overlap {
  %m0 = sea.mem.nondet "m" [0] : !u
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %three = sea.const 3 : i64
  %q = sea.ptr.add %p, %three : !sea.ptr<64>, i64
  %v = sea.const 77 : i32
  %m1 = sea.store %m0[%q], %v inplace : !u, i32
  %one = sea.const 1 : i64
  %r = sea.ptr.add %p, %one : !sea.ptr<64>, i64
  %x = sea.load %m1[%r] : !u -> i32
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
