// RUN: sea-run %s --program=const --input=0=3 --missing=error | FileCheck %s --check-prefix=INT
// RUN: sea-run %s --program=const --input=0=5 --missing=error | FileCheck %s --check-prefix=INT
// RUN: sea-vc %s --program=const | FileCheck %s --check-prefix=VC

// sea.mem.const: every cell holds the value (no input is asked for), until
// it is written.
// INT: status: halted
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
// incoming edge): Yices takes no constant arrays except through selects,
// so the backend inlines the definition (Yices2SolverImpl add).
// RUN: %if yices %{ sea-vc %s --program=join --lower --solver=yices | FileCheck %s --check-prefix=VC %}
// RUN: sea-vc %s --program=join --lower --solver=z3 | FileCheck %s --check-prefix=VC
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

// `shadow` (a tool's metadata memory, sea-lower-tracking): no value depends
// on it; it round-trips.
// RUN: sea-run %s --program=shadow | FileCheck %s --check-prefix=INT
// RUN: sea-opt %s | sea-opt | FileCheck %s --check-prefix=RT
// RT: sea.mem.const %{{.*}} shadow : !sea.mem<!sea.addr<8>, i8>
sea.program @shadow {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z shadow : !sea.mem<!sea.addr<8>, i8>
  %c = sea.const 3 : i8
  %q = sea.addr.from_int %c : i8 to !sea.addr<8>
  %x = sea.load %m0[%q] : !sea.mem<!sea.addr<8>, i8> -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
