// RUN: sea-vc %s --lower --program=imp_safe | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program=sea_safe | FileCheck %s --check-prefix=UNSAT
// RUN: sea-vc %s --lower --program=imp_bug | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program=sea_bug | FileCheck %s --check-prefix=SAT
// RUN: sea-vc %s --lower --program=imp_bug --replay-original | FileCheck %s --check-prefix=ORIG

// A region (a memory variable) gives the verdicts of the same program
// written with explicit memory SSA (docs/design/SeaImp.md).
// UNSAT: result: unsat
// SAT: result: sat
// SAT: replay: assert-failed
// SAT: mismatches=0
// ORIG: replay.original: status=assert-failed {{.*}}match=yes

!m = !sea.mem<!sea.ptr<64>, i8>

// int o; int *p = &o; *p = 5; if (c) *p = 7; assert(*p >= 5)
sea.program @imp_safe {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %c4  = sea.const 4 : i64
  %p   = sea.alloca %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %v5  = sea.const 5 : i32
  seaimp.store %mem[%p], %v5 align 4 : !seaimp.var<!m>, i32
  %c   = sea.nondet "nd_bool" [2] : i1
  sea.cond_br %c, ^bb1, ^bb2
^bb1:
  %v7 = sea.const 7 : i32
  seaimp.store %mem[%p], %v7 align 4 : !seaimp.var<!m>, i32
  sea.br ^bb2
^bb2:
  %x  = seaimp.load %mem[%p] align 4 : !seaimp.var<!m> -> i32
  %ok = sea.icmp sge, %x, %v5 : i32
  sea.assert %ok
  sea.halt
}

sea.program @sea_safe {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c4 = sea.const 4 : i64
  %p  = sea.alloca %c4 align 4 [1] : i64 -> !sea.ptr<64>
  %v5 = sea.const 5 : i32
  %m1 = sea.store %m0[%p], %v5 inplace align 4 : !m, i32
  %c  = sea.nondet "nd_bool" [2] : i1
  sea.cond_br %c, ^bb1, ^bb2(%m1 : !m)
^bb1:
  %v7 = sea.const 7 : i32
  %m2 = sea.store %m1[%p], %v7 inplace align 4 : !m, i32
  sea.br ^bb2(%m2 : !m)
^bb2(%m3 : !m):
  %x  = sea.load %m3[%p] align 4 : !m -> i32
  %ok = sea.icmp sge, %x, %v5 : i32
  sea.assert %ok
  sea.halt
}

// memset(buf, 0, 16); memcpy(dst, buf, 8); havoc(buf, 16);
// assert(dst[0..8) == 0 && buf[0..8) == 0): dst was copied before the
// memhavoc and holds, buf was havocked and fails.
sea.program @imp_bug {
  %mem = seaimp.var "mem" [0] : !seaimp.var<!m>
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %z8 = sea.const 0 : i8
  %buf = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %dst = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  seaimp.memset %mem[%buf], %z8, %c16 : !seaimp.var<!m>
  seaimp.memmove %mem[%dst], %mem[%buf], %c8 : !seaimp.var<!m>
  seaimp.memhavoc %mem[%buf], %c16 [5] : !seaimp.var<!m>
  %x = seaimp.load %mem[%dst] : !seaimp.var<!m> -> i64
  %y = seaimp.load %mem[%buf] : !seaimp.var<!m> -> i64
  %z64 = sea.const 0 : i64
  %okx = sea.icmp eq, %x, %z64 : i64
  %oky = sea.icmp eq, %y, %z64 : i64
  %ok = sea.and %okx, %oky : i1
  sea.assert %ok
  sea.halt
}

sea.program @sea_bug {
  %m0 = sea.mem.nondet "mem" [0] : !m
  %c16 = sea.const 16 : i64
  %c8 = sea.const 8 : i64
  %z8 = sea.const 0 : i8
  %buf = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %dst = sea.alloca %c8 align 8 [2] : i64 -> !sea.ptr<64>
  %m1 = sea.memset %m0[%buf], %z8, %c16 inplace : !m
  %m2 = sea.memmove %m1[%dst], %m1[%buf], %c8 inplace : !m
  %m3 = sea.memhavoc %m2[%buf], %c16 [5] inplace : !m
  %x = sea.load %m3[%dst] : !m -> i64
  %y = sea.load %m3[%buf] : !m -> i64
  %z64 = sea.const 0 : i64
  %okx = sea.icmp eq, %x, %z64 : i64
  %oky = sea.icmp eq, %y, %z64 : i64
  %ok = sea.and %okx, %oky : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
