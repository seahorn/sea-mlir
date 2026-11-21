// RUN: sea-run %s | FileCheck %s

// Memory block arguments swapped on a back edge are bound as a parallel
// copy: after one swap m0 is the all-ones memory and m1 the all-zeros one.
// CHECK: status: halted
!m = !sea.mem<!sea.addr<8>, i8>
sea.program @memswap {
  %z = sea.const 0 : i8
  %o = sea.const 1 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %mz = sea.mem.const %z : !m
  %mo = sea.mem.const %o : !m
  %t = sea.const 1 : i1
  sea.br ^h(%mz, %mo, %t : !m, !m, i1)
^h(%m0 : !m, %m1 : !m, %first : i1):
  %f = sea.const 0 : i1
  sea.cond_br %first, ^h(%m1, %m0, %f : !m, !m, i1), ^use
^use:
  %x = sea.load %m0[%a] : !m -> i8
  %y = sea.load %m1[%a] : !m -> i8
  %c0 = sea.icmp eq, %x, %o : i8
  %c1 = sea.icmp eq, %y, %z : i8
  sea.assert %c0
  sea.assert %c1
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
