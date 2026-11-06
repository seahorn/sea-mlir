// REQUIRES: bitwuzla
// RUN: sea-vc %s --program=const --solver=bitwuzla | FileCheck %s --check-prefix=VC
// RUN: sea-vc %s --program=join --lower --solver=bitwuzla | FileCheck %s --check-prefix=VC
// RUN: sea-vc %s --program=bug --lower --solver=bitwuzla | FileCheck %s --check-prefix=BUG

// Constant memories with Bitwuzla (cf. test/Interp/mem/const.mlir): selects
// over a const-array, and a const-array reaching a join. Bitwuzla 0.9 answers
// unknown on an equality over a const-array, so the backend inlines the
// definitions of memories named at joins (as Yices does, for other reasons).
// VC: result: unsat
sea.program @const {
  %z = sea.const 7 : i8
  %m0 = sea.mem.const %z : !sea.mem<!sea.addr<8>, i8>
  %a = sea.nondet "a" [0] : i8
  %c = sea.const 3 : i8
  %p = sea.addr.from_int %a : i8 to !sea.addr<8>
  %q = sea.addr.from_int %c : i8 to !sea.addr<8>
  %v = sea.const 9 : i8
  %m1 = sea.store %m0[%q], %v inplace : !sea.mem<!sea.addr<8>, i8>, i8
  %x = sea.load %m1[%p] : !sea.mem<!sea.addr<8>, i8> -> i8
  %is3 = sea.icmp eq, %a, %c : i8
  %e = sea.select %is3, %v, %z : i8
  %ok = sea.icmp eq, %x, %e : i8
  sea.assert %ok
  sea.halt
}

// A constant memory reaching a join (the VC names the joined memory per
// incoming edge).
sea.program @join {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !sea.mem<!sea.addr<8>, i8>
  %c = sea.nondet "c" [0] : i1
  %a = sea.nondet "a" [1] : i8
  %p = sea.addr.from_int %a : i8 to !sea.addr<8>
  %v = sea.const 5 : i8
  sea.cond_br %c, ^w, ^j(%m0 : !sea.mem<!sea.addr<8>, i8>)
^w:
  %m1 = sea.store %m0[%p], %v inplace : !sea.mem<!sea.addr<8>, i8>, i8
  sea.br ^j(%m1 : !sea.mem<!sea.addr<8>, i8>)
^j(%m2: !sea.mem<!sea.addr<8>, i8>):
  %x = sea.load %m2[%p] : !sea.mem<!sea.addr<8>, i8> -> i8
  %e = sea.select %c, %v, %z : i8
  %ok = sea.icmp eq, %x, %e : i8
  sea.assert %ok
  sea.halt
}

// The same join with a wrong expectation (cell 0 instead of 5 on the write
// path): sat, and the counterexample is replayed (cells come from the model).
// BUG: result: sat
// BUG: replay: assert-failed
// BUG: validate: checked={{[0-9]+}} mismatches=0
sea.program @bug {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !sea.mem<!sea.addr<8>, i8>
  %c = sea.nondet "c" [0] : i1
  %a = sea.nondet "a" [1] : i8
  %p = sea.addr.from_int %a : i8 to !sea.addr<8>
  %v = sea.const 5 : i8
  sea.cond_br %c, ^w, ^j(%m0 : !sea.mem<!sea.addr<8>, i8>)
^w:
  %m1 = sea.store %m0[%p], %v inplace : !sea.mem<!sea.addr<8>, i8>, i8
  sea.br ^j(%m1 : !sea.mem<!sea.addr<8>, i8>)
^j(%m2: !sea.mem<!sea.addr<8>, i8>):
  %x = sea.load %m2[%p] : !sea.mem<!sea.addr<8>, i8> -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
