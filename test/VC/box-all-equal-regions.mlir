// RUN: sea-vc %s --program=cells --lower --seam --dag-stats > %t.cells
// RUN: sea-vc %s --program=box --lower --seam --box-all-expand=0 --dag-stats > %t.box
// RUN: FileCheck %s --check-prefix=CELLS --input-file=%t.cells
// RUN: FileCheck %s --check-prefix=BOX --input-file=%t.box
// RUN: %python -c "import re, sys; n = lambda p: int(re.search(r'vc.dag-op: ite (\d+)', open(p).read()).group(1)); sys.exit(0 if n(sys.argv[2]) < n(sys.argv[1]) else 1)" %t.cells %t.box
// RUN: sea-vc %s --program=box_bug --lower --seam --box-all-expand=0 --replay-original | FileCheck %s --check-prefix=BUG
// RUN: sea-vc %s --program=box --lower --seam | FileCheck %s --check-prefix=EXPAND
// RUN: sea-vc %s --program=box_bug --lower --seam --replay-original | FileCheck %s --check-prefix=EXPAND-BUG
// RUN: not sea-vc %s --program=unlowered 2>&1 | FileCheck %s --check-prefix=UNLOWERED
// "Two regions are equal" (docs/design/BoxAll.md): 12 bytes from a
// source region at a symbolic offset are copied forwards into a and
// backwards into b (both at symbolic offsets of one object); the claim is
// a[i] == b[i] for every i < 12. Stated cell by cell (@cells), each of the
// 24 reads of the final memory is a chain over the 24 stores; stated as one
// sea.box.all (@box), the lowering reads two cells at one arbitrary index:
// fewer ite terms in the query (the python line). Both unsat. @box_bug
// copies b[5] from src[6]: sat; the replay names the index (the box's
// nondet), and the original program, run on the model, fails at index 5.
// CELLS: result: unsat
// BOX: vc.box-all: dead=0 empty=0 expand-max=0 expanded=0 guarded=0 lowered=1 nondets=1
// BOX: result: unsat
// BUG: vc.box-all: {{.*}}lowered=1
// BUG: result: sat
// BUG: nondet: site={{[0-9]+}} callee=box.all.eq.0 width=64 value=5
// BUG: replay: assert-failed
// BUG: validate: {{.*}} mismatches=0
// BUG: replay.original: status=assert-failed {{.*}}match=yes {{.*}}box-all-false=(5)
// With the default --box-all-expand (12 cells <= K) the box is expanded:
// the same verdicts, no index nondet.
// EXPAND: vc.box-all: {{.*}}expanded=1 {{.*}}lowered=0
// EXPAND: result: unsat
// EXPAND-BUG: vc.box-all: {{.*}}expanded=1
// EXPAND-BUG: result: sat
// EXPAND-BUG-NOT: callee=box.all
// EXPAND-BUG: replay: assert-failed
// EXPAND-BUG: validate: {{.*}} mismatches=0
// EXPAND-BUG: replay.original: status=assert-failed {{.*}}match=yes {{.*}}box-all-false=(5)
// @unlowered: the VC encodes a box only through its lowering.
// UNLOWERED: [vc.box-all-unlowered]
!m = !sea.mem<!sea.ptr<64>, i8>

sea.program @cells {
  %n = sea.const 12 : i64
  %c64 = sea.const 64 : i64
  %c32 = sea.const 32 : i64
  %m0 = sea.mem.nondet "m" [0] : !m
  %srcb = sea.alloca %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %dst = sea.alloca %c64 align 8 [2] : i64 -> !sea.ptr<64>
  %oa = sea.nondet "oa" [3] : i64
  %ob = sea.nondet "ob" [6] : i64
  %c40 = sea.const 40 : i64
  %oae = sea.add %oa, %n : i64
  %o1 = sea.icmp ule, %oae, %ob : i64
  %o2 = sea.icmp ult, %ob, %c40 : i64
  %o3 = sea.icmp ult, %oa, %c40 : i64
  %o12 = sea.and %o1, %o2 : i1
  %o = sea.and %o12, %o3 : i1
  sea.assume %o
  %a = sea.ptr.add %dst, %oa : !sea.ptr<64>, i64
  %b = sea.ptr.add %dst, %ob : !sea.ptr<64>, i64
  %s = sea.nondet "start" [4] : i64
  %sok = sea.icmp ult, %s, %c32 : i64
  sea.assume %sok
  %src = sea.ptr.add %srcb, %s : !sea.ptr<64>, i64
  %c0 = sea.const 0 : i64
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c3 = sea.const 3 : i64
  %c4 = sea.const 4 : i64
  %c5 = sea.const 5 : i64
  %c6 = sea.const 6 : i64
  %c7 = sea.const 7 : i64
  %c8 = sea.const 8 : i64
  %c9 = sea.const 9 : i64
  %c10 = sea.const 10 : i64
  %c11 = sea.const 11 : i64
  %sa0 = sea.ptr.add %src, %c0 : !sea.ptr<64>, i64
  %va0 = sea.load %m0[%sa0] : !m -> i8
  %da0 = sea.ptr.add %a, %c0 : !sea.ptr<64>, i64
  %w1 = sea.store %m0[%da0], %va0 inplace : !m, i8
  %sa1 = sea.ptr.add %src, %c1 : !sea.ptr<64>, i64
  %va1 = sea.load %w1[%sa1] : !m -> i8
  %da1 = sea.ptr.add %a, %c1 : !sea.ptr<64>, i64
  %w2 = sea.store %w1[%da1], %va1 inplace : !m, i8
  %sa2 = sea.ptr.add %src, %c2 : !sea.ptr<64>, i64
  %va2 = sea.load %w2[%sa2] : !m -> i8
  %da2 = sea.ptr.add %a, %c2 : !sea.ptr<64>, i64
  %w3 = sea.store %w2[%da2], %va2 inplace : !m, i8
  %sa3 = sea.ptr.add %src, %c3 : !sea.ptr<64>, i64
  %va3 = sea.load %w3[%sa3] : !m -> i8
  %da3 = sea.ptr.add %a, %c3 : !sea.ptr<64>, i64
  %w4 = sea.store %w3[%da3], %va3 inplace : !m, i8
  %sa4 = sea.ptr.add %src, %c4 : !sea.ptr<64>, i64
  %va4 = sea.load %w4[%sa4] : !m -> i8
  %da4 = sea.ptr.add %a, %c4 : !sea.ptr<64>, i64
  %w5 = sea.store %w4[%da4], %va4 inplace : !m, i8
  %sa5 = sea.ptr.add %src, %c5 : !sea.ptr<64>, i64
  %va5 = sea.load %w5[%sa5] : !m -> i8
  %da5 = sea.ptr.add %a, %c5 : !sea.ptr<64>, i64
  %w6 = sea.store %w5[%da5], %va5 inplace : !m, i8
  %sa6 = sea.ptr.add %src, %c6 : !sea.ptr<64>, i64
  %va6 = sea.load %w6[%sa6] : !m -> i8
  %da6 = sea.ptr.add %a, %c6 : !sea.ptr<64>, i64
  %w7 = sea.store %w6[%da6], %va6 inplace : !m, i8
  %sa7 = sea.ptr.add %src, %c7 : !sea.ptr<64>, i64
  %va7 = sea.load %w7[%sa7] : !m -> i8
  %da7 = sea.ptr.add %a, %c7 : !sea.ptr<64>, i64
  %w8 = sea.store %w7[%da7], %va7 inplace : !m, i8
  %sa8 = sea.ptr.add %src, %c8 : !sea.ptr<64>, i64
  %va8 = sea.load %w8[%sa8] : !m -> i8
  %da8 = sea.ptr.add %a, %c8 : !sea.ptr<64>, i64
  %w9 = sea.store %w8[%da8], %va8 inplace : !m, i8
  %sa9 = sea.ptr.add %src, %c9 : !sea.ptr<64>, i64
  %va9 = sea.load %w9[%sa9] : !m -> i8
  %da9 = sea.ptr.add %a, %c9 : !sea.ptr<64>, i64
  %w10 = sea.store %w9[%da9], %va9 inplace : !m, i8
  %sa10 = sea.ptr.add %src, %c10 : !sea.ptr<64>, i64
  %va10 = sea.load %w10[%sa10] : !m -> i8
  %da10 = sea.ptr.add %a, %c10 : !sea.ptr<64>, i64
  %w11 = sea.store %w10[%da10], %va10 inplace : !m, i8
  %sa11 = sea.ptr.add %src, %c11 : !sea.ptr<64>, i64
  %va11 = sea.load %w11[%sa11] : !m -> i8
  %da11 = sea.ptr.add %a, %c11 : !sea.ptr<64>, i64
  %w12 = sea.store %w11[%da11], %va11 inplace : !m, i8
  %sb11 = sea.ptr.add %src, %c11 : !sea.ptr<64>, i64
  %vb11 = sea.load %w12[%sb11] : !m -> i8
  %db11 = sea.ptr.add %b, %c11 : !sea.ptr<64>, i64
  %w13 = sea.store %w12[%db11], %vb11 inplace : !m, i8
  %sb10 = sea.ptr.add %src, %c10 : !sea.ptr<64>, i64
  %vb10 = sea.load %w13[%sb10] : !m -> i8
  %db10 = sea.ptr.add %b, %c10 : !sea.ptr<64>, i64
  %w14 = sea.store %w13[%db10], %vb10 inplace : !m, i8
  %sb9 = sea.ptr.add %src, %c9 : !sea.ptr<64>, i64
  %vb9 = sea.load %w14[%sb9] : !m -> i8
  %db9 = sea.ptr.add %b, %c9 : !sea.ptr<64>, i64
  %w15 = sea.store %w14[%db9], %vb9 inplace : !m, i8
  %sb8 = sea.ptr.add %src, %c8 : !sea.ptr<64>, i64
  %vb8 = sea.load %w15[%sb8] : !m -> i8
  %db8 = sea.ptr.add %b, %c8 : !sea.ptr<64>, i64
  %w16 = sea.store %w15[%db8], %vb8 inplace : !m, i8
  %sb7 = sea.ptr.add %src, %c7 : !sea.ptr<64>, i64
  %vb7 = sea.load %w16[%sb7] : !m -> i8
  %db7 = sea.ptr.add %b, %c7 : !sea.ptr<64>, i64
  %w17 = sea.store %w16[%db7], %vb7 inplace : !m, i8
  %sb6 = sea.ptr.add %src, %c6 : !sea.ptr<64>, i64
  %vb6 = sea.load %w17[%sb6] : !m -> i8
  %db6 = sea.ptr.add %b, %c6 : !sea.ptr<64>, i64
  %w18 = sea.store %w17[%db6], %vb6 inplace : !m, i8
  %sb5 = sea.ptr.add %src, %c5 : !sea.ptr<64>, i64
  %vb5 = sea.load %w18[%sb5] : !m -> i8
  %db5 = sea.ptr.add %b, %c5 : !sea.ptr<64>, i64
  %w19 = sea.store %w18[%db5], %vb5 inplace : !m, i8
  %sb4 = sea.ptr.add %src, %c4 : !sea.ptr<64>, i64
  %vb4 = sea.load %w19[%sb4] : !m -> i8
  %db4 = sea.ptr.add %b, %c4 : !sea.ptr<64>, i64
  %w20 = sea.store %w19[%db4], %vb4 inplace : !m, i8
  %sb3 = sea.ptr.add %src, %c3 : !sea.ptr<64>, i64
  %vb3 = sea.load %w20[%sb3] : !m -> i8
  %db3 = sea.ptr.add %b, %c3 : !sea.ptr<64>, i64
  %w21 = sea.store %w20[%db3], %vb3 inplace : !m, i8
  %sb2 = sea.ptr.add %src, %c2 : !sea.ptr<64>, i64
  %vb2 = sea.load %w21[%sb2] : !m -> i8
  %db2 = sea.ptr.add %b, %c2 : !sea.ptr<64>, i64
  %w22 = sea.store %w21[%db2], %vb2 inplace : !m, i8
  %sb1 = sea.ptr.add %src, %c1 : !sea.ptr<64>, i64
  %vb1 = sea.load %w22[%sb1] : !m -> i8
  %db1 = sea.ptr.add %b, %c1 : !sea.ptr<64>, i64
  %w23 = sea.store %w22[%db1], %vb1 inplace : !m, i8
  %sb0 = sea.ptr.add %src, %c0 : !sea.ptr<64>, i64
  %vb0 = sea.load %w23[%sb0] : !m -> i8
  %db0 = sea.ptr.add %b, %c0 : !sea.ptr<64>, i64
  %w24 = sea.store %w23[%db0], %vb0 inplace : !m, i8
  %xa0 = sea.load %w24[%da0] : !m -> i8
  %xb0 = sea.load %w24[%db0] : !m -> i8
  %e0 = sea.icmp eq, %xa0, %xb0 : i8
  %xa1 = sea.load %w24[%da1] : !m -> i8
  %xb1 = sea.load %w24[%db1] : !m -> i8
  %e1 = sea.icmp eq, %xa1, %xb1 : i8
  %and1 = sea.and %e0, %e1 : i1
  %xa2 = sea.load %w24[%da2] : !m -> i8
  %xb2 = sea.load %w24[%db2] : !m -> i8
  %e2 = sea.icmp eq, %xa2, %xb2 : i8
  %and2 = sea.and %and1, %e2 : i1
  %xa3 = sea.load %w24[%da3] : !m -> i8
  %xb3 = sea.load %w24[%db3] : !m -> i8
  %e3 = sea.icmp eq, %xa3, %xb3 : i8
  %and3 = sea.and %and2, %e3 : i1
  %xa4 = sea.load %w24[%da4] : !m -> i8
  %xb4 = sea.load %w24[%db4] : !m -> i8
  %e4 = sea.icmp eq, %xa4, %xb4 : i8
  %and4 = sea.and %and3, %e4 : i1
  %xa5 = sea.load %w24[%da5] : !m -> i8
  %xb5 = sea.load %w24[%db5] : !m -> i8
  %e5 = sea.icmp eq, %xa5, %xb5 : i8
  %and5 = sea.and %and4, %e5 : i1
  %xa6 = sea.load %w24[%da6] : !m -> i8
  %xb6 = sea.load %w24[%db6] : !m -> i8
  %e6 = sea.icmp eq, %xa6, %xb6 : i8
  %and6 = sea.and %and5, %e6 : i1
  %xa7 = sea.load %w24[%da7] : !m -> i8
  %xb7 = sea.load %w24[%db7] : !m -> i8
  %e7 = sea.icmp eq, %xa7, %xb7 : i8
  %and7 = sea.and %and6, %e7 : i1
  %xa8 = sea.load %w24[%da8] : !m -> i8
  %xb8 = sea.load %w24[%db8] : !m -> i8
  %e8 = sea.icmp eq, %xa8, %xb8 : i8
  %and8 = sea.and %and7, %e8 : i1
  %xa9 = sea.load %w24[%da9] : !m -> i8
  %xb9 = sea.load %w24[%db9] : !m -> i8
  %e9 = sea.icmp eq, %xa9, %xb9 : i8
  %and9 = sea.and %and8, %e9 : i1
  %xa10 = sea.load %w24[%da10] : !m -> i8
  %xb10 = sea.load %w24[%db10] : !m -> i8
  %e10 = sea.icmp eq, %xa10, %xb10 : i8
  %and10 = sea.and %and9, %e10 : i1
  %xa11 = sea.load %w24[%da11] : !m -> i8
  %xb11 = sea.load %w24[%db11] : !m -> i8
  %e11 = sea.icmp eq, %xa11, %xb11 : i8
  %and11 = sea.and %and10, %e11 : i1
  sea.assert %and11
  sea.halt
}

sea.program @box {
  %n = sea.const 12 : i64
  %c64 = sea.const 64 : i64
  %c32 = sea.const 32 : i64
  %m0 = sea.mem.nondet "m" [0] : !m
  %srcb = sea.alloca %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %dst = sea.alloca %c64 align 8 [2] : i64 -> !sea.ptr<64>
  %oa = sea.nondet "oa" [3] : i64
  %ob = sea.nondet "ob" [6] : i64
  %c40 = sea.const 40 : i64
  %oae = sea.add %oa, %n : i64
  %o1 = sea.icmp ule, %oae, %ob : i64
  %o2 = sea.icmp ult, %ob, %c40 : i64
  %o3 = sea.icmp ult, %oa, %c40 : i64
  %o12 = sea.and %o1, %o2 : i1
  %o = sea.and %o12, %o3 : i1
  sea.assume %o
  %a = sea.ptr.add %dst, %oa : !sea.ptr<64>, i64
  %b = sea.ptr.add %dst, %ob : !sea.ptr<64>, i64
  %s = sea.nondet "start" [4] : i64
  %sok = sea.icmp ult, %s, %c32 : i64
  sea.assume %sok
  %src = sea.ptr.add %srcb, %s : !sea.ptr<64>, i64
  %c0 = sea.const 0 : i64
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c3 = sea.const 3 : i64
  %c4 = sea.const 4 : i64
  %c5 = sea.const 5 : i64
  %c6 = sea.const 6 : i64
  %c7 = sea.const 7 : i64
  %c8 = sea.const 8 : i64
  %c9 = sea.const 9 : i64
  %c10 = sea.const 10 : i64
  %c11 = sea.const 11 : i64
  %sa0 = sea.ptr.add %src, %c0 : !sea.ptr<64>, i64
  %va0 = sea.load %m0[%sa0] : !m -> i8
  %da0 = sea.ptr.add %a, %c0 : !sea.ptr<64>, i64
  %w1 = sea.store %m0[%da0], %va0 inplace : !m, i8
  %sa1 = sea.ptr.add %src, %c1 : !sea.ptr<64>, i64
  %va1 = sea.load %w1[%sa1] : !m -> i8
  %da1 = sea.ptr.add %a, %c1 : !sea.ptr<64>, i64
  %w2 = sea.store %w1[%da1], %va1 inplace : !m, i8
  %sa2 = sea.ptr.add %src, %c2 : !sea.ptr<64>, i64
  %va2 = sea.load %w2[%sa2] : !m -> i8
  %da2 = sea.ptr.add %a, %c2 : !sea.ptr<64>, i64
  %w3 = sea.store %w2[%da2], %va2 inplace : !m, i8
  %sa3 = sea.ptr.add %src, %c3 : !sea.ptr<64>, i64
  %va3 = sea.load %w3[%sa3] : !m -> i8
  %da3 = sea.ptr.add %a, %c3 : !sea.ptr<64>, i64
  %w4 = sea.store %w3[%da3], %va3 inplace : !m, i8
  %sa4 = sea.ptr.add %src, %c4 : !sea.ptr<64>, i64
  %va4 = sea.load %w4[%sa4] : !m -> i8
  %da4 = sea.ptr.add %a, %c4 : !sea.ptr<64>, i64
  %w5 = sea.store %w4[%da4], %va4 inplace : !m, i8
  %sa5 = sea.ptr.add %src, %c5 : !sea.ptr<64>, i64
  %va5 = sea.load %w5[%sa5] : !m -> i8
  %da5 = sea.ptr.add %a, %c5 : !sea.ptr<64>, i64
  %w6 = sea.store %w5[%da5], %va5 inplace : !m, i8
  %sa6 = sea.ptr.add %src, %c6 : !sea.ptr<64>, i64
  %va6 = sea.load %w6[%sa6] : !m -> i8
  %da6 = sea.ptr.add %a, %c6 : !sea.ptr<64>, i64
  %w7 = sea.store %w6[%da6], %va6 inplace : !m, i8
  %sa7 = sea.ptr.add %src, %c7 : !sea.ptr<64>, i64
  %va7 = sea.load %w7[%sa7] : !m -> i8
  %da7 = sea.ptr.add %a, %c7 : !sea.ptr<64>, i64
  %w8 = sea.store %w7[%da7], %va7 inplace : !m, i8
  %sa8 = sea.ptr.add %src, %c8 : !sea.ptr<64>, i64
  %va8 = sea.load %w8[%sa8] : !m -> i8
  %da8 = sea.ptr.add %a, %c8 : !sea.ptr<64>, i64
  %w9 = sea.store %w8[%da8], %va8 inplace : !m, i8
  %sa9 = sea.ptr.add %src, %c9 : !sea.ptr<64>, i64
  %va9 = sea.load %w9[%sa9] : !m -> i8
  %da9 = sea.ptr.add %a, %c9 : !sea.ptr<64>, i64
  %w10 = sea.store %w9[%da9], %va9 inplace : !m, i8
  %sa10 = sea.ptr.add %src, %c10 : !sea.ptr<64>, i64
  %va10 = sea.load %w10[%sa10] : !m -> i8
  %da10 = sea.ptr.add %a, %c10 : !sea.ptr<64>, i64
  %w11 = sea.store %w10[%da10], %va10 inplace : !m, i8
  %sa11 = sea.ptr.add %src, %c11 : !sea.ptr<64>, i64
  %va11 = sea.load %w11[%sa11] : !m -> i8
  %da11 = sea.ptr.add %a, %c11 : !sea.ptr<64>, i64
  %w12 = sea.store %w11[%da11], %va11 inplace : !m, i8
  %sb11 = sea.ptr.add %src, %c11 : !sea.ptr<64>, i64
  %vb11 = sea.load %w12[%sb11] : !m -> i8
  %db11 = sea.ptr.add %b, %c11 : !sea.ptr<64>, i64
  %w13 = sea.store %w12[%db11], %vb11 inplace : !m, i8
  %sb10 = sea.ptr.add %src, %c10 : !sea.ptr<64>, i64
  %vb10 = sea.load %w13[%sb10] : !m -> i8
  %db10 = sea.ptr.add %b, %c10 : !sea.ptr<64>, i64
  %w14 = sea.store %w13[%db10], %vb10 inplace : !m, i8
  %sb9 = sea.ptr.add %src, %c9 : !sea.ptr<64>, i64
  %vb9 = sea.load %w14[%sb9] : !m -> i8
  %db9 = sea.ptr.add %b, %c9 : !sea.ptr<64>, i64
  %w15 = sea.store %w14[%db9], %vb9 inplace : !m, i8
  %sb8 = sea.ptr.add %src, %c8 : !sea.ptr<64>, i64
  %vb8 = sea.load %w15[%sb8] : !m -> i8
  %db8 = sea.ptr.add %b, %c8 : !sea.ptr<64>, i64
  %w16 = sea.store %w15[%db8], %vb8 inplace : !m, i8
  %sb7 = sea.ptr.add %src, %c7 : !sea.ptr<64>, i64
  %vb7 = sea.load %w16[%sb7] : !m -> i8
  %db7 = sea.ptr.add %b, %c7 : !sea.ptr<64>, i64
  %w17 = sea.store %w16[%db7], %vb7 inplace : !m, i8
  %sb6 = sea.ptr.add %src, %c6 : !sea.ptr<64>, i64
  %vb6 = sea.load %w17[%sb6] : !m -> i8
  %db6 = sea.ptr.add %b, %c6 : !sea.ptr<64>, i64
  %w18 = sea.store %w17[%db6], %vb6 inplace : !m, i8
  %sb5 = sea.ptr.add %src, %c5 : !sea.ptr<64>, i64
  %vb5 = sea.load %w18[%sb5] : !m -> i8
  %db5 = sea.ptr.add %b, %c5 : !sea.ptr<64>, i64
  %w19 = sea.store %w18[%db5], %vb5 inplace : !m, i8
  %sb4 = sea.ptr.add %src, %c4 : !sea.ptr<64>, i64
  %vb4 = sea.load %w19[%sb4] : !m -> i8
  %db4 = sea.ptr.add %b, %c4 : !sea.ptr<64>, i64
  %w20 = sea.store %w19[%db4], %vb4 inplace : !m, i8
  %sb3 = sea.ptr.add %src, %c3 : !sea.ptr<64>, i64
  %vb3 = sea.load %w20[%sb3] : !m -> i8
  %db3 = sea.ptr.add %b, %c3 : !sea.ptr<64>, i64
  %w21 = sea.store %w20[%db3], %vb3 inplace : !m, i8
  %sb2 = sea.ptr.add %src, %c2 : !sea.ptr<64>, i64
  %vb2 = sea.load %w21[%sb2] : !m -> i8
  %db2 = sea.ptr.add %b, %c2 : !sea.ptr<64>, i64
  %w22 = sea.store %w21[%db2], %vb2 inplace : !m, i8
  %sb1 = sea.ptr.add %src, %c1 : !sea.ptr<64>, i64
  %vb1 = sea.load %w22[%sb1] : !m -> i8
  %db1 = sea.ptr.add %b, %c1 : !sea.ptr<64>, i64
  %w23 = sea.store %w22[%db1], %vb1 inplace : !m, i8
  %sb0 = sea.ptr.add %src, %c0 : !sea.ptr<64>, i64
  %vb0 = sea.load %w23[%sb0] : !m -> i8
  %db0 = sea.ptr.add %b, %c0 : !sea.ptr<64>, i64
  %w24 = sea.store %w23[%db0], %vb0 inplace : !m, i8
  %eq = sea.box.all (%i) = (0) to (%n) {
    %pa = sea.ptr.add %a, %i : !sea.ptr<64>, i64
    %pb = sea.ptr.add %b, %i : !sea.ptr<64>, i64
    %xa = sea.load %w24[%pa] : !m -> i8
    %xb = sea.load %w24[%pb] : !m -> i8
    %e = sea.icmp eq, %xa, %xb : i8
    sea.yield %e
  } loc("eq")
  sea.assert %eq
  sea.halt
}

sea.program @box_bug {
  %n = sea.const 12 : i64
  %c64 = sea.const 64 : i64
  %c32 = sea.const 32 : i64
  %m0 = sea.mem.nondet "m" [0] : !m
  %srcb = sea.alloca %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %dst = sea.alloca %c64 align 8 [2] : i64 -> !sea.ptr<64>
  %oa = sea.nondet "oa" [3] : i64
  %ob = sea.nondet "ob" [6] : i64
  %c40 = sea.const 40 : i64
  %oae = sea.add %oa, %n : i64
  %o1 = sea.icmp ule, %oae, %ob : i64
  %o2 = sea.icmp ult, %ob, %c40 : i64
  %o3 = sea.icmp ult, %oa, %c40 : i64
  %o12 = sea.and %o1, %o2 : i1
  %o = sea.and %o12, %o3 : i1
  sea.assume %o
  %a = sea.ptr.add %dst, %oa : !sea.ptr<64>, i64
  %b = sea.ptr.add %dst, %ob : !sea.ptr<64>, i64
  %s = sea.nondet "start" [4] : i64
  %sok = sea.icmp ult, %s, %c32 : i64
  sea.assume %sok
  %src = sea.ptr.add %srcb, %s : !sea.ptr<64>, i64
  %c0 = sea.const 0 : i64
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c3 = sea.const 3 : i64
  %c4 = sea.const 4 : i64
  %c5 = sea.const 5 : i64
  %c6 = sea.const 6 : i64
  %c7 = sea.const 7 : i64
  %c8 = sea.const 8 : i64
  %c9 = sea.const 9 : i64
  %c10 = sea.const 10 : i64
  %c11 = sea.const 11 : i64
  %sa0 = sea.ptr.add %src, %c0 : !sea.ptr<64>, i64
  %va0 = sea.load %m0[%sa0] : !m -> i8
  %da0 = sea.ptr.add %a, %c0 : !sea.ptr<64>, i64
  %w1 = sea.store %m0[%da0], %va0 inplace : !m, i8
  %sa1 = sea.ptr.add %src, %c1 : !sea.ptr<64>, i64
  %va1 = sea.load %w1[%sa1] : !m -> i8
  %da1 = sea.ptr.add %a, %c1 : !sea.ptr<64>, i64
  %w2 = sea.store %w1[%da1], %va1 inplace : !m, i8
  %sa2 = sea.ptr.add %src, %c2 : !sea.ptr<64>, i64
  %va2 = sea.load %w2[%sa2] : !m -> i8
  %da2 = sea.ptr.add %a, %c2 : !sea.ptr<64>, i64
  %w3 = sea.store %w2[%da2], %va2 inplace : !m, i8
  %sa3 = sea.ptr.add %src, %c3 : !sea.ptr<64>, i64
  %va3 = sea.load %w3[%sa3] : !m -> i8
  %da3 = sea.ptr.add %a, %c3 : !sea.ptr<64>, i64
  %w4 = sea.store %w3[%da3], %va3 inplace : !m, i8
  %sa4 = sea.ptr.add %src, %c4 : !sea.ptr<64>, i64
  %va4 = sea.load %w4[%sa4] : !m -> i8
  %da4 = sea.ptr.add %a, %c4 : !sea.ptr<64>, i64
  %w5 = sea.store %w4[%da4], %va4 inplace : !m, i8
  %sa5 = sea.ptr.add %src, %c5 : !sea.ptr<64>, i64
  %va5 = sea.load %w5[%sa5] : !m -> i8
  %da5 = sea.ptr.add %a, %c5 : !sea.ptr<64>, i64
  %w6 = sea.store %w5[%da5], %va5 inplace : !m, i8
  %sa6 = sea.ptr.add %src, %c6 : !sea.ptr<64>, i64
  %va6 = sea.load %w6[%sa6] : !m -> i8
  %da6 = sea.ptr.add %a, %c6 : !sea.ptr<64>, i64
  %w7 = sea.store %w6[%da6], %va6 inplace : !m, i8
  %sa7 = sea.ptr.add %src, %c7 : !sea.ptr<64>, i64
  %va7 = sea.load %w7[%sa7] : !m -> i8
  %da7 = sea.ptr.add %a, %c7 : !sea.ptr<64>, i64
  %w8 = sea.store %w7[%da7], %va7 inplace : !m, i8
  %sa8 = sea.ptr.add %src, %c8 : !sea.ptr<64>, i64
  %va8 = sea.load %w8[%sa8] : !m -> i8
  %da8 = sea.ptr.add %a, %c8 : !sea.ptr<64>, i64
  %w9 = sea.store %w8[%da8], %va8 inplace : !m, i8
  %sa9 = sea.ptr.add %src, %c9 : !sea.ptr<64>, i64
  %va9 = sea.load %w9[%sa9] : !m -> i8
  %da9 = sea.ptr.add %a, %c9 : !sea.ptr<64>, i64
  %w10 = sea.store %w9[%da9], %va9 inplace : !m, i8
  %sa10 = sea.ptr.add %src, %c10 : !sea.ptr<64>, i64
  %va10 = sea.load %w10[%sa10] : !m -> i8
  %da10 = sea.ptr.add %a, %c10 : !sea.ptr<64>, i64
  %w11 = sea.store %w10[%da10], %va10 inplace : !m, i8
  %sa11 = sea.ptr.add %src, %c11 : !sea.ptr<64>, i64
  %va11 = sea.load %w11[%sa11] : !m -> i8
  %da11 = sea.ptr.add %a, %c11 : !sea.ptr<64>, i64
  %w12 = sea.store %w11[%da11], %va11 inplace : !m, i8
  %sb11 = sea.ptr.add %src, %c11 : !sea.ptr<64>, i64
  %vb11 = sea.load %w12[%sb11] : !m -> i8
  %db11 = sea.ptr.add %b, %c11 : !sea.ptr<64>, i64
  %w13 = sea.store %w12[%db11], %vb11 inplace : !m, i8
  %sb10 = sea.ptr.add %src, %c10 : !sea.ptr<64>, i64
  %vb10 = sea.load %w13[%sb10] : !m -> i8
  %db10 = sea.ptr.add %b, %c10 : !sea.ptr<64>, i64
  %w14 = sea.store %w13[%db10], %vb10 inplace : !m, i8
  %sb9 = sea.ptr.add %src, %c9 : !sea.ptr<64>, i64
  %vb9 = sea.load %w14[%sb9] : !m -> i8
  %db9 = sea.ptr.add %b, %c9 : !sea.ptr<64>, i64
  %w15 = sea.store %w14[%db9], %vb9 inplace : !m, i8
  %sb8 = sea.ptr.add %src, %c8 : !sea.ptr<64>, i64
  %vb8 = sea.load %w15[%sb8] : !m -> i8
  %db8 = sea.ptr.add %b, %c8 : !sea.ptr<64>, i64
  %w16 = sea.store %w15[%db8], %vb8 inplace : !m, i8
  %sb7 = sea.ptr.add %src, %c7 : !sea.ptr<64>, i64
  %vb7 = sea.load %w16[%sb7] : !m -> i8
  %db7 = sea.ptr.add %b, %c7 : !sea.ptr<64>, i64
  %w17 = sea.store %w16[%db7], %vb7 inplace : !m, i8
  %sb6 = sea.ptr.add %src, %c6 : !sea.ptr<64>, i64
  %vb6 = sea.load %w17[%sb6] : !m -> i8
  %db6 = sea.ptr.add %b, %c6 : !sea.ptr<64>, i64
  %w18 = sea.store %w17[%db6], %vb6 inplace : !m, i8
  %sb5 = sea.ptr.add %src, %c6 : !sea.ptr<64>, i64
  %vb5 = sea.load %w18[%sb5] : !m -> i8
  %db5 = sea.ptr.add %b, %c5 : !sea.ptr<64>, i64
  %w19 = sea.store %w18[%db5], %vb5 inplace : !m, i8
  %sb4 = sea.ptr.add %src, %c4 : !sea.ptr<64>, i64
  %vb4 = sea.load %w19[%sb4] : !m -> i8
  %db4 = sea.ptr.add %b, %c4 : !sea.ptr<64>, i64
  %w20 = sea.store %w19[%db4], %vb4 inplace : !m, i8
  %sb3 = sea.ptr.add %src, %c3 : !sea.ptr<64>, i64
  %vb3 = sea.load %w20[%sb3] : !m -> i8
  %db3 = sea.ptr.add %b, %c3 : !sea.ptr<64>, i64
  %w21 = sea.store %w20[%db3], %vb3 inplace : !m, i8
  %sb2 = sea.ptr.add %src, %c2 : !sea.ptr<64>, i64
  %vb2 = sea.load %w21[%sb2] : !m -> i8
  %db2 = sea.ptr.add %b, %c2 : !sea.ptr<64>, i64
  %w22 = sea.store %w21[%db2], %vb2 inplace : !m, i8
  %sb1 = sea.ptr.add %src, %c1 : !sea.ptr<64>, i64
  %vb1 = sea.load %w22[%sb1] : !m -> i8
  %db1 = sea.ptr.add %b, %c1 : !sea.ptr<64>, i64
  %w23 = sea.store %w22[%db1], %vb1 inplace : !m, i8
  %sb0 = sea.ptr.add %src, %c0 : !sea.ptr<64>, i64
  %vb0 = sea.load %w23[%sb0] : !m -> i8
  %db0 = sea.ptr.add %b, %c0 : !sea.ptr<64>, i64
  %w24 = sea.store %w23[%db0], %vb0 inplace : !m, i8
  %eq = sea.box.all (%i) = (0) to (%n) {
    %pa = sea.ptr.add %a, %i : !sea.ptr<64>, i64
    %pb = sea.ptr.add %b, %i : !sea.ptr<64>, i64
    %xa = sea.load %w24[%pa] : !m -> i8
    %xb = sea.load %w24[%pb] : !m -> i8
    %e = sea.icmp eq, %xa, %xb : i8
    sea.yield %e
  } loc("eq")
  sea.assert %eq
  sea.halt
}

sea.program @unlowered {
  %c4 = sea.const 4 : i64
  %all = sea.box.all (%i) = (0) to (%c4) {
    %e = sea.icmp ult, %i, %c4 : i64
    sea.yield %e
  }
  sea.assert %all
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
