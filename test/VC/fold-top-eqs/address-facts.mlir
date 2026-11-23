// The fold of --vc-fold-top-eqs and facts on addresses
// (docs/design/FoldTopEqs.md, "Pointer observations"; PtrSegment.md, "What
// F2 does then"): a fact whose term mentions an object is skipped
// (skipped-base), one that mentions none is substituted, pointer
// observations included. (--lower-disable=assume-fold: sea-assume-fold
// would replace the integer's uses before the VC.)
// RUN: sea-vc %s --program=reg --lower --lower-disable=assume-fold --seam --smt2 2>%t.reg | FileCheck %s --check-prefix=REG
// RUN: FileCheck %s --check-prefix=REG-STATS < %t.reg
// RUN: sea-vc %s --program=obj --lower --lower-disable=assume-fold --seam --smt2 2>%t.obj | FileCheck %s --check-prefix=OBJ
// RUN: FileCheck %s --check-prefix=OBJ-STATS < %t.obj
// RUN: sea-vc %s --program=obj --lower --lower-disable=assume-fold --seam --smt2 --x-fold-top-eqs-no-base-skip 2>%t.noskip | FileCheck %s --check-prefix=REG
// RUN: FileCheck %s --check-prefix=NOSKIP-STATS < %t.noskip
//
// @reg: a loaded pointer p (no object: its address is memory contents)
// with to_int(p) = 8192 assumed; the fact is substituted and the assertion
// folds, the query is false.
// REG: (assert false)
// REG-STATS: vc.fold-top-eqs: {{.*}}uses={{[1-9][0-9]*}} {{.*}}skipped-base=0
//
// @obj: the same fact on to_int(a + x), a pointer into the object a: the
// term mentions a, so the fact is skipped and the product stays (the
// testing switch substitutes it).
// OBJ-NOT: (assert false)
// OBJ: bvmul
// OBJ-STATS: vc.fold-top-eqs: {{.*}}skipped-base={{[1-9][0-9]*}}
// NOSKIP-STATS: [vc.fold-top-eqs-no-base-skip]
// NOSKIP-STATS: vc.fold-top-eqs: {{.*}}uses={{[1-9][0-9]*}} {{.*}}skipped-base=0
!am = !sea.mem<!sea.addr<32>, i8>
sea.program @reg {
^bb0:
  %a = sea.segment "heap.1" base 4096 : i32 size 16 : i32 align 16 kind "heap" : !sea.addr<32>
  %m = sea.mem.nondet "m" [0] : !am
  %p = sea.load %m[%a] : !am -> !sea.addr<32>
  %i = sea.addr.to_int %p : !sea.addr<32> to i32
  %k = sea.const 8192 : i32
  %c = sea.icmp eq, %i, %k : i32
  sea.assume %c
  %y = sea.nondet "y" [1] : i32
  %j = sea.mul %i, %y : i32
  %k2 = sea.mul %k, %y : i32
  %f = sea.icmp eq, %j, %k2 : i32
  sea.assert %f
  sea.halt
}
sea.program @obj {
^bb0:
  %a = sea.segment "heap.1" base 4096 : i32 size 16 : i32 align 16 kind "heap" : !sea.addr<32>
  %x = sea.nondet "x" [0] : i32
  %p = sea.addr.add %a, %x : !sea.addr<32>, i32
  %i = sea.addr.to_int %p : !sea.addr<32> to i32
  %k = sea.const 4100 : i32
  %c = sea.icmp eq, %i, %k : i32
  sea.assume %c
  %y = sea.nondet "y" [1] : i32
  %j = sea.mul %i, %y : i32
  %k2 = sea.mul %k, %y : i32
  %f = sea.icmp eq, %j, %k2 : i32
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
