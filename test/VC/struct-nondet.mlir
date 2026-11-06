// RUN: sea-run %s --input=0=0x050005 | FileCheck %s --check-prefix=STATUS
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VC

// A nondet struct is one nondet bit-vector of its integer leaves (low bits
// first; here i16 then i8 then i1: 25 bits); pointer leaves are null. The
// input 0x050005 makes both i8 views 5.
// STATUS: nondet: site=0 callee=undef width=25 value=327685
// STATUS: status: assert-failed
// VC: result: sat
// VC: nondet: site=0 callee=undef width=25
// VC: replay: assert-failed
sea.program @main {
  %s = sea.struct.nondet "undef" [0] : !sea.struct<i16, !sea.ptr<32>, !sea.struct<i8, i1>>
  %x = sea.struct.extract %s[0] : !sea.struct<i16, !sea.ptr<32>, !sea.struct<i8, i1>> -> i16
  %p = sea.struct.extract %s[1] : !sea.struct<i16, !sea.ptr<32>, !sea.struct<i8, i1>> -> !sea.ptr<32>
  %n = sea.struct.extract %s[2] : !sea.struct<i16, !sea.ptr<32>, !sea.struct<i8, i1>> -> !sea.struct<i8, i1>
  %y = sea.struct.extract %n[0] : !sea.struct<i8, i1> -> i8
  %null = sea.ptr.null : !sea.ptr<32>
  %isnull = sea.ptr.cmp eq, %p, %null : !sea.ptr<32>
  %xl = sea.trunc %x : i16 to i8
  %ne = sea.icmp ne, %xl, %y : i8
  %ok = sea.and %ne, %isnull : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
