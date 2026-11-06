// RUN: sea-opt %s | sea-opt | FileCheck %s --check-prefix=RT
// RUN: sea-opt %s --sea-place-static | FileCheck %s --check-prefix=PLACED
// RUN: sea-run %s --program=ok --missing=zero 2>&1 | FileCheck %s --check-prefix=OK
// RUN: sea-run %s --program=offset --missing=zero --log=interp.align-promise 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: sea-run %s --program=object --missing=zero --log=interp.align-promise 2>&1 | FileCheck %s --check-prefix=OBJ
// RUN: sea-run %s --program=placed --missing=zero --log=interp.align-promise 2>&1 | FileCheck %s --check-prefix=ADDR

// `align` (bytes, LLVM's) is a promise: a broken one is reported, the run
// goes on.

!pm = !sea.mem<!sea.ptr<64>, i8>

// RT-LABEL: sea.program @ok
// RT: sea.store {{.*}} inplace align 8 :
// RT: sea.load {{.*}} align 8 :
// RT: sea.memset {{.*}} inplace align 4 :
// RT: sea.memmove {{.*}} inplace align 8 src_align 8 :
// Placement keeps the promises.
// PLACED-LABEL: sea.program @ok
// PLACED: sea.load {{.*}} align 8 : !sea.mem<!sea.addr<64>, i8>
// OK: status: halted
// OK-NOT: align-promise
sea.program @ok {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %eight = sea.const 8 : i64
  %q = sea.ptr.add %p, %eight : !sea.ptr<64>, i64
  %v = sea.const 7 : i64
  %m1 = sea.store %m0[%q], %v inplace align 8 : !pm, i64
  %x = sea.load %m1[%q] align 8 : !pm -> i64
  %z = sea.const 0 : i8
  %m2 = sea.memset %m1[%p], %z, %eight inplace align 4 : !pm
  %m3 = sea.memmove %m2[%p], %m2[%q], %eight inplace align 8 src_align 8 : !pm
  sea.halt
}

// OFF: sea.load align 4: the promise is broken (offset 2 in stack.1 is not a multiple of it)
// OFF: status: halted
sea.program @offset {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %two = sea.const 2 : i64
  %q = sea.ptr.add %p, %two : !sea.ptr<64>, i64
  %x = sea.load %m0[%q] align 4 : !pm -> i32
  sea.halt
}

// OBJ: sea.memmove src_align 8: the promise is broken (stack.2 is aligned to 1 only)
sea.program @object {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 8 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %q = sea.alloca %n align 1 [2] : i64 -> !sea.ptr<64>
  %m1 = sea.memmove %m0[%p], %m0[%q], %n inplace align 8 src_align 8 : !pm
  sea.halt
}

// ADDR: sea.store align 2: the promise is broken (the address 5 is not a multiple of it)
sea.program @placed {
  %m0 = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
  %a = sea.const 5 : i64
  %p = sea.addr.from_int %a : i64 to !sea.addr<64>
  %v = sea.const 1 : i16
  %m1 = sea.store %m0[%p], %v inplace align 2 : !sea.mem<!sea.addr<64>, i8>, i16
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
