// RUN: sea-run %s --program=unused 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=compared 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=hoisted --input=1=0 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=hoisted --input=1=1 2>&1 | FileCheck %s --check-prefix=LOAD
// RUN: sea-run %s --program=empty 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=select --input=1=0 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=select --input=1=1 2>&1 | FileCheck %s --check-prefix=LOAD
// RUN: sea-run %s --program=chain 2>&1 | FileCheck %s --check-prefix=STORE
// RUN: sea-run %s --program=phi 2>&1 | FileCheck %s --check-prefix=LOAD

// A broken ptr.index promise is poison in LLVM, UB only when the pointer is
// the address of an access (docs/design/PtrIndex.md, "Where the promise is
// a fact"). sea-run warns interp.ptr-index-promise at an executed load,
// store, or memset / memmove of nonzero length whose address comes from the
// broken GEP, not where the GEP is computed. In every program %q = 1 + 6 *
// 24 breaks P1 (144 does not fit 8 signed bits). 8-bit addresses.
// QUIET-NOT: interp.ptr-index-promise
// QUIET: status: halted
// LOAD: sea.ptr.index at {{.*}} nusw: a product wraps (P1): the promise is broken{{.*}} this sea.load uses it{{.*}}[interp.ptr-index-promise]
// STORE: sea.ptr.index at {{.*}} nusw: a product wraps (P1): the promise is broken{{.*}} this sea.store uses it{{.*}}[interp.ptr-index-promise]
!am = !sea.mem<!sea.addr<8>, i8>

// computed, never used
sea.program @unused {
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 6 : i8
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  sea.halt
}

// only compared
sea.program @compared {
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 6 : i8
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %c = sea.addr.cmp ne, %q, %a : !sea.addr<8>
  sea.assert %c
  sea.halt
}

// hoisted out of its guard (as LLVM does): the load through it runs only
// when the guard %g holds
sea.program @hoisted {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !am
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 6 : i8
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %g = sea.nondet "g" [1] : i1
  sea.cond_br %g, ^use, ^skip
^use:
  %x = sea.load %m[%q] : !am -> i8
  sea.br ^skip
^skip:
  sea.halt
}

// a memset of length 0 accesses nothing
sea.program @empty {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !am
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 6 : i8
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %m1 = sea.memset %m[%q], %z, %z inplace : !am
  sea.halt
}

// the select picks the broken GEP only when %s is 1
sea.program @select {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !am
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 6 : i8
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %s = sea.nondet "s" [1] : i1
  %p = sea.select %s, %q, %a : !sea.addr<8>
  %x = sea.load %m[%p] : !am -> i8
  sea.halt
}

// a field of an element: the outer GEP and the addr.add keep their
// promises, the store's address comes from the broken GEP through them
sea.program @chain {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !am
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 6 : i8
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %f = sea.ptr.index %q [] strides [] offset 2 nusw : !sea.addr<8>
  %one = sea.const 1 : i8
  %g = sea.addr.add %f, %one : !sea.addr<8>, i8
  %m1 = sea.store %m[%g], %z inplace : !am, i8
  sea.halt
}

// through a block argument (a phi)
sea.program @phi {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !am
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 6 : i8
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  sea.br ^j(%q : !sea.addr<8>)
^j(%p : !sea.addr<8>):
  %x = sea.load %m[%p] : !am -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
