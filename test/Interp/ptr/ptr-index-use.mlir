// RUN: sea-run %s --program=unused 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=compared 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=hoisted --input=1=0 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=hoisted --input=1=1 2>&1 | FileCheck %s --check-prefix=LOAD
// RUN: sea-run %s --program=empty 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=select --input=1=0 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=select --input=1=1 2>&1 | FileCheck %s --check-prefix=LOAD
// RUN: sea-run %s --program=chain 2>&1 | FileCheck %s --check-prefix=STORE
// RUN: sea-run %s --program=phi 2>&1 | FileCheck %s --check-prefix=LOAD
// RUN: sea-run %s --program=stale 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=swap 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=swapbad 2>&1 | FileCheck %s --check-prefix=LOAD
// RUN: sea-run %s --program=mmsrc 2>&1 | FileCheck %s --check-prefix=MM
// RUN: sea-run %s --program=mmzero 2>&1 | FileCheck %s --check-prefix=QUIET
//
// Through memory (16-bit addresses: a stored address spans two cells).
// RUN: sea-run %s --program=viaaddr 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-opt %s --sea-split-wide-access -o %t.split.mlir
// RUN: sea-run %t.split.mlir --program=viaaddr 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %t.split.mlir --program=identity 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=overwritten 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=partial 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=moved 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=memset 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=havoc --missing=zero 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=intonly 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=memphi 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=identity 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=idsub 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=subleft 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: sea-run %s --program=idxor 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=idand 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=idmul 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=andself 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=trunc 2>&1 | FileCheck %s --check-prefix=MEM
// RUN: sea-run %s --program=arith 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: not sea-run %s --program=twosets --max-poison-sets=1 2>&1 | FileCheck %s --check-prefix=CAP
// RUN: not sea-run %s --program=unused --max-poison-sets=65536 2>&1 | FileCheck %s --check-prefix=CAPFLAG

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
// MM: sea.ptr.index at {{.*}} nusw: a product wraps (P1): the promise is broken{{.*}} this sea.memmove uses it{{.*}}[interp.ptr-index-promise]
// MEM: sea.ptr.index at {{.*}} nusw: a product wraps (P1): the promise is broken (a poison pointer in LLVM), the pointer went through memory, and this sea.load uses it{{.*}}[interp.ptr-index-promise]
// CAP: status: error
// CAP: message: more than 1 distinct sets of broken ptr.index promises
// CAPFLAG: --max-poison-sets must be at most 65535
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


// iteration 1: i = 6 breaks the promise, the access is skipped; iteration
// 2: i = 1 keeps it, and the access runs
sea.program @stale {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !am
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %six = sea.const 6 : i8
  %one = sea.const 1 : i8
  sea.br ^h(%six : i8)
^h(%i : i8):
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %r = sea.addr.add %q, %z : !sea.addr<8>, i8
  %brk = sea.icmp eq, %i, %six : i8
  sea.cond_br %brk, ^h(%one : i8), ^use
^use:
  %x = sea.load %m[%r] : !am -> i8
  sea.halt
}

// block arguments swap once (a parallel copy): %p0 ends as %a, %p1 as %q
sea.program @swap {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !am
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %six = sea.const 6 : i8
  %q = sea.ptr.index %a [%six] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %t = sea.const 1 : i1
  sea.br ^h(%q, %a, %t : !sea.addr<8>, !sea.addr<8>, i1)
^h(%p0 : !sea.addr<8>, %p1 : !sea.addr<8>, %first : i1):
  %f = sea.const 0 : i1
  sea.cond_br %first, ^h(%p1, %p0, %f : !sea.addr<8>, !sea.addr<8>, i1), ^use
^use:
  %x = sea.load %m[%p0] : !am -> i8
  sea.halt
}
sea.program @swapbad {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !am
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %six = sea.const 6 : i8
  %q = sea.ptr.index %a [%six] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %t = sea.const 1 : i1
  sea.br ^h(%q, %a, %t : !sea.addr<8>, !sea.addr<8>, i1)
^h(%p0 : !sea.addr<8>, %p1 : !sea.addr<8>, %first : i1):
  %f = sea.const 0 : i1
  sea.cond_br %first, ^h(%p1, %p0, %f : !sea.addr<8>, !sea.addr<8>, i1), ^use
^use:
  %x = sea.load %m[%p1] : !am -> i8
  sea.halt
}

// the broken GEP as a memmove source: of length 0 (nothing accessed), of 1
sea.program @mmzero {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !am
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %six = sea.const 6 : i8
  %q = sea.ptr.index %a [%six] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %m1 = sea.memmove %m[%a], %m[%q], %z inplace : !am
  sea.halt
}
sea.program @mmsrc {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !am
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %six = sea.const 6 : i8
  %one = sea.const 1 : i8
  %q = sea.ptr.index %a [%six] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %m1 = sea.memmove %m[%a], %m[%q], %one inplace : !am
  sea.halt
}

// Through memory. The broken pointer is stored in two cells at %s = 100
// (16-bit addresses), and a load reads it back: still poison in LLVM, and
// SeaM's ptr.load-resolve sees the GEP through the store, so the stride
// rule may rely on its promise there. %q = 1 + 2000 * 24 breaks P1
// (48000 does not fit 16 signed bits).
!wm = !sea.mem<!sea.addr<16>, i8>

// direct form; split by sea-split-wide-access (to_bits, extract, cell
// stores; cell loads, concat, from_bits) in the second RUN
sea.program @viaaddr {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %m1 = sea.store %m[%s], %q inplace : !wm, !sea.addr<16>
  %r = sea.load %m1[%s] : !wm -> !sea.addr<16>
  %x = sea.load %m1[%r] : !wm -> i8
  sea.halt
}

// stored over by an intact address: the mark is gone
sea.program @overwritten {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %m1 = sea.store %m[%s], %q inplace : !wm, !sea.addr<16>
  %m2 = sea.store %m1[%s], %a inplace : !wm, !sea.addr<16>
  %r = sea.load %m2[%s] : !wm -> !sea.addr<16>
  %x = sea.load %m2[%r] : !wm -> i8
  sea.halt
}

// one of the two cells overwritten by an integer: the other still marks
// the loaded address (a poison byte poisons the value)
sea.program @partial {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %m1 = sea.store %m[%s], %q inplace : !wm, !sea.addr<16>
  %c1 = sea.const 1 : i16
  %s1 = sea.addr.add %s, %c1 : !sea.addr<16>, i16
  %m2 = sea.store %m1[%s1], %z inplace : !wm, i8
  %r = sea.load %m2[%s] : !wm -> !sea.addr<16>
  %x = sea.load %m2[%r] : !wm -> i8
  sea.halt
}

// memmove carries the mark to the copy at 200
sea.program @moved {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %tb = sea.const 200 : i16
  %t = sea.addr.from_int %tb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %m1 = sea.store %m[%s], %q inplace : !wm, !sea.addr<16>
  %two = sea.const 2 : i16
  %m2 = sea.memmove %m1[%t], %m1[%s], %two inplace : !wm
  %r = sea.load %m2[%t] : !wm -> !sea.addr<16>
  %x = sea.load %m2[%r] : !wm -> i8
  sea.halt
}

// memset and memhavoc write fresh cells: the mark is gone
sea.program @memset {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %m1 = sea.store %m[%s], %q inplace : !wm, !sea.addr<16>
  %two = sea.const 2 : i16
  %m2 = sea.memset %m1[%s], %z, %two inplace : !wm
  %r = sea.load %m2[%s] : !wm -> !sea.addr<16>
  %x = sea.load %m2[%r] : !wm -> i8
  sea.halt
}
sea.program @havoc {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %m1 = sea.store %m[%s], %q inplace : !wm, !sea.addr<16>
  %two = sea.const 2 : i16
  %m2 = sea.memhavoc %m1[%s], %two [7] inplace : !wm
  %r = sea.load %m2[%s] : !wm -> !sea.addr<16>
  %x = sea.load %m2[%r] : !wm -> i8
  sea.halt
}

// the stored bytes read back as an integer and only compared
sea.program @intonly {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %m1 = sea.store %m[%s], %q inplace : !wm, !sea.addr<16>
  %v = sea.load %m1[%s] : !wm -> i16
  %z16 = sea.const 0 : i16
  %c = sea.icmp ne, %v, %z16 : i16
  sea.assert %c
  sea.halt
}

// the memory passes a block argument between the store and the load
sea.program @memphi {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %m1 = sea.store %m[%s], %q inplace : !wm, !sea.addr<16>
  sea.br ^j(%m1 : !wm)
^j(%mj : !wm):
  %r = sea.load %mj[%s] : !wm -> !sea.addr<16>
  %x = sea.load %mj[%r] : !wm -> i8
  sea.halt
}

// the bits pass x | 0 on the way to memory: SeaM drops the unit
// (bv.ac.or) and resolves the load to the GEP when the lowering keeps the
// or (sea-vc --lower-opt=false), so the mark passes it; x + 2 is
// arithmetic and is not followed
sea.program @identity {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %bits = sea.addr.to_bits %q : !sea.addr<16> to i16
  %z16 = sea.const 0 : i16
  %or = sea.or %bits, %z16 : i16
  %m1 = sea.store %m[%s], %or inplace : !wm, i16
  %v = sea.load %m1[%s] : !wm -> i16
  %r = sea.addr.from_bits %v : i16 to !sea.addr<16>
  %x = sea.load %m1[%r] : !wm -> i8
  sea.halt
}
// @identity with x - 0
sea.program @idsub {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %bits = sea.addr.to_bits %q : !sea.addr<16> to i16
  %z16 = sea.const 0 : i16
  %or = sea.sub %bits, %z16 : i16
  %m1 = sea.store %m[%s], %or inplace : !wm, i16
  %v = sea.load %m1[%s] : !wm -> i16
  %r = sea.addr.from_bits %v : i16 to !sea.addr<16>
  %x = sea.load %m1[%r] : !wm -> i8
  sea.halt
}

// @identity with 0 - x: a left 0 is not a unit of sub
sea.program @subleft {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %bits = sea.addr.to_bits %q : !sea.addr<16> to i16
  %z16 = sea.const 0 : i16
  %or = sea.sub %z16, %bits : i16
  %m1 = sea.store %m[%s], %or inplace : !wm, i16
  %v = sea.load %m1[%s] : !wm -> i16
  %r = sea.addr.from_bits %v : i16 to !sea.addr<16>
  %x = sea.load %m1[%r] : !wm -> i8
  sea.halt
}

// @identity with x ^ 0
sea.program @idxor {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %bits = sea.addr.to_bits %q : !sea.addr<16> to i16
  %z16 = sea.const 0 : i16
  %or = sea.xor %bits, %z16 : i16
  %m1 = sea.store %m[%s], %or inplace : !wm, i16
  %v = sea.load %m1[%s] : !wm -> i16
  %r = sea.addr.from_bits %v : i16 to !sea.addr<16>
  %x = sea.load %m1[%r] : !wm -> i8
  sea.halt
}

// @identity with x & ~0
sea.program @idand {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %bits = sea.addr.to_bits %q : !sea.addr<16> to i16
  %ones = sea.const -1 : i16
  %or = sea.and %bits, %ones : i16
  %m1 = sea.store %m[%s], %or inplace : !wm, i16
  %v = sea.load %m1[%s] : !wm -> i16
  %r = sea.addr.from_bits %v : i16 to !sea.addr<16>
  %x = sea.load %m1[%r] : !wm -> i8
  sea.halt
}

// @identity with x * 1
sea.program @idmul {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %bits = sea.addr.to_bits %q : !sea.addr<16> to i16
  %c1 = sea.const 1 : i16
  %or = sea.mul %bits, %c1 : i16
  %m1 = sea.store %m[%s], %or inplace : !wm, i16
  %v = sea.load %m1[%s] : !wm -> i16
  %r = sea.addr.from_bits %v : i16 to !sea.addr<16>
  %x = sea.load %m1[%r] : !wm -> i8
  sea.halt
}

// @identity with x & x
sea.program @andself {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %bits = sea.addr.to_bits %q : !sea.addr<16> to i16
  %or = sea.and %bits, %bits : i16
  %m1 = sea.store %m[%s], %or inplace : !wm, i16
  %v = sea.load %m1[%s] : !wm -> i16
  %r = sea.addr.from_bits %v : i16 to !sea.addr<16>
  %x = sea.load %m1[%r] : !wm -> i8
  sea.halt
}

// @identity with the low half of concat(0, x), by trunc
sea.program @trunc {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %bits = sea.addr.to_bits %q : !sea.addr<16> to i16
  %z16 = sea.const 0 : i16
  %w = sea.concat %z16, %bits : i16, i16 -> i32
  %or = sea.trunc %w : i32 to i16
  %m1 = sea.store %m[%s], %or inplace : !wm, i16
  %v = sea.load %m1[%s] : !wm -> i16
  %r = sea.addr.from_bits %v : i16 to !sea.addr<16>
  %x = sea.load %m1[%r] : !wm -> i8
  sea.halt
}

sea.program @arith {
  %z = sea.const 0 : i8
  %m = sea.mem.const %z : !wm
  %b = sea.const 1 : i16
  %a = sea.addr.from_int %b : i16 to !sea.addr<16>
  %sb = sea.const 100 : i16
  %s = sea.addr.from_int %sb : i16 to !sea.addr<16>
  %i = sea.const 2000 : i16
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<16>, i16
  %bits = sea.addr.to_bits %q : !sea.addr<16> to i16
  %two = sea.const 2 : i16
  %add = sea.add %bits, %two : i16
  %m1 = sea.store %m[%s], %add inplace : !wm, i16
  %v = sea.load %m1[%s] : !wm -> i16
  %r = sea.addr.from_bits %v : i16 to !sea.addr<16>
  %x = sea.load %m1[%r] : !wm -> i8
  sea.halt
}

// two broken GEPs make two sets: past --max-poison-sets=1 the run stops
sea.program @twosets {
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %six = sea.const 6 : i8
  %q = sea.ptr.index %a [%six] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  %q2 = sea.ptr.index %a [%six] strides [25] offset 0 inbounds : !sea.addr<8>, i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
