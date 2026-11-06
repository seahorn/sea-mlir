// RUN: sea-run %s --program=values --trace 2>&1 | FileCheck %s --check-prefix=VAL
// RUN: sea-run %s --program=abstract --missing=zero 2>&1 | FileCheck %s --check-prefix=ABS
// RUN: sea-run %s --program=p1 2>&1 | FileCheck %s --check-prefix=P1
// RUN: sea-run %s --program=p2 2>&1 | FileCheck %s --check-prefix=P2
// RUN: sea-run %s --program=p3 2>&1 | FileCheck %s --check-prefix=P3
// RUN: sea-run %s --program=p3under 2>&1 | FileCheck %s --check-prefix=P3
// RUN: sea-run %s --program=nuw 2>&1 | FileCheck %s --check-prefix=NUW
// RUN: sea-run %s --program=values 2>&1 | FileCheck %s --check-prefix=QUIET

// sea.ptr.index (docs/design/PtrIndex.md): the value is
// base + sum sext(i_k)*s_k + c (mod 2^n); a broken promise is a warning
// (interp.ptr-index-promise), never a stop. 8-bit addresses.
!am = !sea.mem<!sea.addr<8>, i8>
!pm = !sea.mem<!sea.ptr<64>, i8>

// 100 + 3*24 + sext(-2:i4)*(-4) + 16 = 196, promise kept (inbounds);
// a negative stride and offset: 100 + 3*(-8) - 20 = 56; the full 8-bit
// range of a stride (-128) and offset (127): 100 + 1*(-128) + 127 = 99
// VAL: value: sea.ptr.index {{.*}} = 196
// VAL: value: sea.ptr.index {{.*}} = 56
// VAL: value: sea.ptr.index {{.*}} = 99
// QUIET-NOT: interp.ptr-index-promise
// QUIET: status: halted
sea.program @values {
  %b = sea.const 100 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 3 : i8
  %j = sea.const -2 : i4
  %k = sea.const 1 : i4
  %q = sea.ptr.index %a [%i, %j] strides [24, -4] offset 16 inbounds : !sea.addr<8>, i8, i4
  %qi = sea.addr.to_int %q : !sea.addr<8> to i8
  %r = sea.ptr.index %a [%i] strides [-8] offset -20 nusw : !sea.addr<8>, i8
  %ri = sea.addr.to_int %r : !sea.addr<8> to i8
  %s = sea.ptr.index %a [%k] strides [-128] offset 127 : !sea.addr<8>, i4
  %si = sea.addr.to_int %s : !sea.addr<8> to i8
  sea.halt
}

// an abstract pointer: the offset moves inside the object; a store through
// p[i].f (i = 2, stride 8, field 4: offset 20) and through p + 24 - 4 meet
// ABS-NOT: interp.ptr-index-promise
// ABS: status: halted
sea.program @abstract {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 64 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %two = sea.const 2 : i64
  %f = sea.ptr.index %p [%two] strides [8] offset 4 inbounds : !sea.ptr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%f], %v inplace : !pm, i8
  %c24 = sea.const 24 : i64
  %p24 = sea.ptr.add %p, %c24 : !sea.ptr<64>, i64
  %three = sea.const 1 : i64
  %g = sea.ptr.index %p24 [%three] strides [-4] offset 0 inbounds : !sea.ptr<64>, i64
  %x = sea.load %m1[%g] : !pm -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok
  sea.halt
}

// promise P1 broken: 6 * 24 = 144 does not fit 8 signed bits
// P1: sea.ptr.index nusw: a product wraps (P1): the promise is broken{{.*}}[interp.ptr-index-promise]
// P1: status: halted
sea.program @p1 {
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 6 : i8
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  sea.halt
}

// promise P2 broken: 5*24 = 120 and (-2)*(-4) = 8 fit; their sum 128
// does not
// P2: sea.ptr.index nusw: the offset wraps (P2): the promise is broken
sea.program @p2 {
  %b = sea.const 1 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 5 : i8
  %j = sea.const -2 : i8
  %q = sea.ptr.index %a [%i, %j] strides [24, -4] offset 0 nusw : !sea.addr<8>, i8, i8
  sea.halt
}

// promise P3 broken: 200 + 72 = 272 leaves the space above; 10 - 16 =
// -6 below
// P3: sea.ptr.index nusw: base + offset leaves the space (P3): the promise is broken
sea.program @p3 {
  %b = sea.const 200 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const 3 : i8
  %q = sea.ptr.index %a [%i] strides [24] offset 0 inbounds : !sea.addr<8>, i8
  sea.halt
}
sea.program @p3under {
  %b = sea.const 10 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const -1 : i8
  %q = sea.ptr.index %a [%i] strides [16] offset 0 inbounds : !sea.addr<8>, i8
  sea.halt
}

// under nuw the index -1 is 255 unsigned; 255 * 2 wraps 8 unsigned bits
// NUW: sea.ptr.index nuw: a product wraps (P1): the promise is broken
sea.program @nuw {
  %b = sea.const 0 : i8
  %a = sea.addr.from_int %b : i8 to !sea.addr<8>
  %i = sea.const -1 : i8
  %q = sea.ptr.index %a [%i] strides [2] offset 0 nuw : !sea.addr<8>, i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
