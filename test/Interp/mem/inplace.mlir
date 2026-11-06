// RUN: not sea-run %s --program=not_inplace --missing=zero 2>&1 | FileCheck %s --check-prefix=NOT
// RUN: not sea-run %s --program=twice --missing=zero 2>&1 | FileCheck %s --check-prefix=TWICE
// RUN: sea-run %s --program=diamond --input=1=1 --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-run %s --program=diamond --input=1=0 --missing=zero | FileCheck %s --check-prefix=OK
// RUN: not sea-run %s --program=stale_branch --missing=zero 2>&1 | FileCheck %s --check-prefix=TWICE

!m8 = !sea.mem<!sea.addr<8>, i8>

// NOT: message: memory update without inplace
sea.program @not_inplace {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%a], %z : !m8, i8
  sea.halt
}

// TWICE: message: memory version consumed twice
sea.program @twice {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %m1 = sea.store %m0[%a], %z inplace : !m8, i8
  %m2 = sea.store %m0[%a], %z inplace : !m8, i8
  sea.halt
}

// Each branch consumes the version once: fine. The join's argument is the
// next version of the same chain.
// OK: status: halted
sea.program @diamond {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %c = sea.nondet "nondet_bool" [1] : i1
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  %one = sea.const 1 : i8
  %two = sea.const 2 : i8
  sea.cond_br %c, ^t, ^e
^t:
  %m1 = sea.store %m0[%a], %one inplace : !m8, i8
  sea.br ^j(%m1 : !m8)
^e:
  %m2 = sea.store %m0[%a], %two inplace : !m8, i8
  sea.br ^j(%m2 : !m8)
^j(%m3 : !m8):
  %x = sea.load %m3[%a] : !m8 -> i8
  %nz = sea.icmp ne, %x, %z : i8
  sea.assert %nz
  sea.halt
}

// Passing a version to a block consumes it; storing it afterwards is the
// second consumer.
sea.program @stale_branch {
  %m0 = sea.mem.nondet "m" [0] : !m8
  %z = sea.const 0 : i8
  %a = sea.addr.from_int %z : i8 to !sea.addr<8>
  sea.br ^j(%m0 : !m8)
^j(%m1 : !m8):
  %m2 = sea.store %m0[%a], %z inplace : !m8, i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
