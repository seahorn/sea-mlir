// The out-of-object warning names every premise a reported access breaks
// when it executes, whatever class it lands in.
// @wrap: an 8-bit address space, object [16, 32), an access at offset -32:
// address 240, but 16 + (-32) < 0, so it wraps (A-NOWRAP) while landing
// in a gap. @nullsym: null + i with a symbolic i lands `unknown`, and every
// execution of it accesses null (A-NULL).
// RUN: sea-vc %s --program=wrap --seam 2>&1 | FileCheck %s --check-prefix=WRAP
// RUN: sea-vc %s --program=nullsym --lower --lower-opt=false --seam 2>&1 | FileCheck %s --check-prefix=NULL
// RUN: not sea-run %s --program=nullsym --input=1=5 2>/dev/null | FileCheck %s --check-prefix=INTERP
// WRAP: Warning: 2 access(es) {{.*}}gap=2);{{.*}}A-NOWRAP breaks if {{.*}}[vc.seam-out-of-object]
// WRAP: seam.out-of-object: accesses=2 object=0 shared=0 unknown=0 null=0 gap=2
// NULL: Warning: 2 access(es) {{.*}}unknown=2 null=0 gap=0);{{.*}}A-NULL breaks if {{.*}}[vc.seam-out-of-object]
// NULL: seam.out-of-object: accesses=2 object=0 shared=0 unknown=2 null=0 gap=0
// INTERP: message: memory access through the null pointer
!am = !sea.mem<!sea.addr<8>, i8>
sea.program @wrap {
  %a = sea.segment "heap.1" base 16 : i8 size 16 : i8 align 1 kind "heap" : !sea.addr<8>
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !am
  %v7 = sea.const 7 : i8
  %o = sea.const -32 : i8
  %p = sea.addr.add %a, %o : !sea.addr<8>, i8
  %m1 = sea.store %m0[%p], %v7 inplace : !am, i8
  %x = sea.load %m1[%p] : !am -> i8
  %ok = sea.icmp eq, %x, %v7 : i8
  sea.assert %ok
  sea.halt
}

!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @nullsym {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.ptr.null : !sea.ptr<64>
  %i = sea.nondet "i" [1] : i64
  %p = sea.ptr.add %n, %i : !sea.ptr<64>, i64
  %x = sea.load %m0[%p] : !m -> i8
  %y = sea.load %m0[%p] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
