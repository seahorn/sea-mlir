// The fold of --vc-fold-top-eqs does not substitute the address of a
// pointer (the layout facts addr(obj.k) = c; docs/design/FoldTopEqs.md,
// "Pointer observations"). The program is utils/fuzz-seam.py --seed 5
// --accesses=wide, program 2 (test/VC/seam/fuzz-check.test): a memmove read
// equation reads at obj.5 + (addr(q) - addr(obj.4)); with the layout facts
// substituted one leaf became a pointer into obj.5, and cmp.diff-base
// decided its alias questions in the fold's round, outside any access:
// four firings (ids 175-181) --seam-check could not state.
// RUN: sea-vc %s --lower --seam --seam-check 2>&1 | FileCheck %s
// CHECK: vc.fold-top-eqs-status: on
// CHECK: vc.fold-top-eqs: {{.*}}skipped-ptr={{[1-9][0-9]*}}
// CHECK: result: sat
// CHECK: seam-check: {{.*}}unexpressible=0
sea.program @"main" {
^bb0:
  %v1 = sea.mem.nondet "r" [0] : !sea.mem<!sea.ptr<32>, i8>
  %v2 = sea.const 32 : i32
  %v3 = sea.alloca %v2 align 1 [1] : i32 -> !sea.ptr<32>
  %v4 = sea.const 32 : i32
  %v5 = sea.alloca %v4 align 1 [2] : i32 -> !sea.ptr<32>
  %v6 = sea.const 16 : i32
  %v7 = sea.alloca %v6 align 1 [3] : i32 -> !sea.ptr<32>
  %v8 = sea.const 16 : i32
  %v9 = sea.alloca %v8 align 1 [4] : i32 -> !sea.ptr<32>
  %v10 = sea.const 32 : i32
  %v11 = sea.alloca %v10 align 1 [5] : i32 -> !sea.ptr<32>
  %v12 = sea.nondet "c" [6] : i1
  %v13 = sea.select %v12, %v9, %v7 : !sea.ptr<32>
  %v14 = sea.nondet "c" [7] : i1
  %v15 = sea.select %v14, %v7, %v13 : !sea.ptr<32>
  %v16 = sea.nondet "c" [8] : i1
  %v17 = sea.select %v16, %v11, %v15 : !sea.ptr<32>
  %v18 = sea.nondet "c" [9] : i1
  %v19 = sea.select %v18, %v7, %v17 : !sea.ptr<32>
  %v20 = sea.nondet "c" [10] : i1
  %v21 = sea.select %v20, %v7, %v19 : !sea.ptr<32>
  %v22 = sea.nondet "c" [11] : i1
  %v23 = sea.select %v22, %v7, %v21 : !sea.ptr<32>
  %v24 = sea.nondet "i" [12] : i32
  %v25 = sea.ptr.add %v23, %v24 : !sea.ptr<32>, i32
  %v26 = sea.nondet "c" [13] : i1
  %v27 = sea.select %v26, %v5, %v7 : !sea.ptr<32>
  %v28 = sea.nondet "c" [14] : i1
  %v29 = sea.select %v28, %v9, %v27 : !sea.ptr<32>
  %v30 = sea.nondet "c" [15] : i1
  %v31 = sea.select %v30, %v7, %v29 : !sea.ptr<32>
  %v32 = sea.nondet "c" [16] : i1
  %v33 = sea.select %v32, %v7, %v31 : !sea.ptr<32>
  %v34 = sea.nondet "c" [17] : i1
  %v35 = sea.select %v34, %v11, %v33 : !sea.ptr<32>
  %v36 = sea.nondet "c" [18] : i1
  %v37 = sea.select %v36, %v5, %v35 : !sea.ptr<32>
  %v38 = sea.nondet "c" [19] : i1
  %v39 = sea.select %v38, %v11, %v37 : !sea.ptr<32>
  %v40 = sea.nondet "i" [20] : i32
  %v41 = sea.const 1 : i32
  %v42 = sea.const 32 : i32
  %v43 = sea.icmp uge, %v40, %v41 : i32
  %v44 = sea.icmp ule, %v40, %v42 : i32
  %v45 = sea.and %v43, %v44 : i1
  sea.assume %v45
  %v46 = sea.ptr.add %v39, %v40 : !sea.ptr<32>, i32
  %v47 = sea.const 1 : i32
  %v48 = sea.store %v1[%v7], %v47 inplace align 1 : !sea.mem<!sea.ptr<32>, i8>, i32
  %v49 = sea.const 8 : i32
  %v50 = sea.ptr.add %v46, %v49 : !sea.ptr<32>, i32
  %v51 = sea.ptr.to_int %v46 : !sea.ptr<32> to i32
  %v52 = sea.ptr.to_int %v11 : !sea.ptr<32> to i32
  %v53 = sea.icmp ult, %v51, %v52 : i32
  %v54 = sea.load %v48[%v25] align 1 : !sea.mem<!sea.ptr<32>, i8> -> i32
  %v55 = sea.nondet "i" [21] : i32
  %v56 = sea.const 2 : i32
  %v57 = sea.const 13 : i32
  %v58 = sea.icmp uge, %v55, %v56 : i32
  %v59 = sea.icmp ule, %v55, %v57 : i32
  %v60 = sea.and %v58, %v59 : i1
  sea.assume %v60
  %v61 = sea.ptr.add %v3, %v55 : !sea.ptr<32>, i32
  %v62 = sea.ptr.to_int %v7 : !sea.ptr<32> to i32
  %v63 = sea.ptr.to_int %v3 : !sea.ptr<32> to i32
  %v64 = sea.icmp eq, %v62, %v63 : i32
  %v65 = sea.const 8 : i32
  %v66 = sea.ptr.add %v3, %v65 : !sea.ptr<32>, i32
  %v67 = sea.store %v48[%v46], %v3 inplace align 1 : !sea.mem<!sea.ptr<32>, i8>, !sea.ptr<32>
  %v69 = sea.const 191 : i32
  %v68 = sea.store %v67[%v11], %v69 inplace align 1 : !sea.mem<!sea.ptr<32>, i8>, i32
  %v70 = sea.const 8 : i32
  %v71 = sea.ptr.add %v11, %v70 : !sea.ptr<32>, i32
  %v72 = sea.ptr.to_int %v46 : !sea.ptr<32> to i32
  %v73 = sea.ptr.to_int %v46 : !sea.ptr<32> to i32
  %v74 = sea.icmp ult, %v72, %v73 : i32
  %v75 = sea.load %v68[%v46] align 1 : !sea.mem<!sea.ptr<32>, i8> -> !sea.ptr<32>
  %v76 = sea.load %v68[%v25] align 1 : !sea.mem<!sea.ptr<32>, i8> -> i32
  %v77 = sea.ptr.cmp ult, %v46, %v5 : !sea.ptr<32>
  %v78 = sea.load %v68[%v46] align 1 : !sea.mem<!sea.ptr<32>, i8> -> i32
  %v79 = sea.const 8 : i32
  %v82 = sea.const 0 : i32
  %v81 = sea.ptr.add %v9, %v82 : !sea.ptr<32>, i32
  %v80 = sea.memmove %v68[%v81], %v68[%v11], %v79 inplace : !sea.mem<!sea.ptr<32>, i8>
  %v84 = sea.const 4 : i32
  %v83 = sea.ptr.add %v9, %v84 : !sea.ptr<32>, i32
  %v85 = sea.nondet "c" [22] : i1
  %v86 = sea.select %v85, %v83, %v11 : !sea.ptr<32>
  %v87 = sea.load %v80[%v86] align 1 : !sea.mem<!sea.ptr<32>, i8> -> i32
  %v88 = sea.const 544892 : i32
  %v89 = sea.icmp ne, %v87, %v88 : i32
  sea.assume %v89
  %v90 = sea.ptr.cmp eq, %v11, %v46 : !sea.ptr<32>
  %v91 = sea.const 8 : i32
  %v94 = sea.const 0 : i32
  %v93 = sea.ptr.add %v7, %v94 : !sea.ptr<32>, i32
  %v95 = sea.const 227 : i8
  %v92 = sea.memset %v80[%v93], %v95, %v91 inplace : !sea.mem<!sea.ptr<32>, i8>
  %v97 = sea.const 4 : i32
  %v96 = sea.ptr.add %v7, %v97 : !sea.ptr<32>, i32
  %v98 = sea.nondet "c" [23] : i1
  %v99 = sea.select %v98, %v96, %v5 : !sea.ptr<32>
  %v100 = sea.load %v92[%v99] align 1 : !sea.mem<!sea.ptr<32>, i8> -> i32
  %v101 = sea.const 286136 : i32
  %v102 = sea.icmp ne, %v100, %v101 : i32
  sea.assume %v102
  %v103 = sea.ptr.cmp ne, %v46, %v3 : !sea.ptr<32>
  %v105 = sea.const 1 : i32
  %v104 = sea.load %v92[%v7] align 1 : !sea.mem<!sea.ptr<32>, i8> -> i32
  %v106 = sea.icmp eq, %v104, %v105 : i32
  sea.assert %v106
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
