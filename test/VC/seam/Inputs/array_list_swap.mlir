// Exported from SeaHorn, function main.
sea.program @"main" {
^bb0:
  %v0 = sea.mem.nondet "region.0" [0] : !sea.mem<!sea.ptr<64>, i8> loc("sm18")
  %v1 = sea.mem.nondet "region.17" [1] : !sea.mem<!sea.ptr<64>, i8> loc("sm19")
  %v2 = sea.mem.nondet "region.16" [2] : !sea.mem<!sea.ptr<64>, i8> loc("sm20")
  %v3 = sea.const 128 : i64 loc("_0")
  %v4 = sea.alloca %v3 align 16 [3] : i64 -> !sea.ptr<64> loc("_0")
  %v5 = sea.const 40 : i64 loc("_1")
  %v6 = sea.alloca %v5 align 8 [4] : i64 -> !sea.ptr<64> loc("_1")
  %v7 = sea.global @"s_allocator_static" size 40 align 8 : !sea.ptr<64>
  %v8 = sea.func_addr @"s_malloc_allocator.addr" : !sea.ptr<64>
  %v9 = sea.store %v0[%v7], %v8 inplace align 8 : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64>
  %v10 = sea.global @"s_allocator_static" size 40 align 8 : !sea.ptr<64> loc("_2")
  %v11 = sea.const 8 : i64 loc("_2")
  %v12 = sea.ptr.add %v10, %v11 : !sea.ptr<64>, i64 loc("_2")
  %v13 = sea.func_addr @"s_free_allocator.addr" : !sea.ptr<64>
  %v14 = sea.store %v9[%v12], %v13 inplace align 8 : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64>
  %v15 = sea.global @"s_allocator_static" size 40 align 8 : !sea.ptr<64> loc("_3")
  %v16 = sea.const 16 : i64 loc("_3")
  %v17 = sea.ptr.add %v15, %v16 : !sea.ptr<64>, i64 loc("_3")
  %v18 = sea.const 0 : i8
  %v19 = sea.const 24 : i64
  %v20 = sea.memset %v14[%v17], %v18, %v19 inplace align 8 : !sea.mem<!sea.ptr<64>, i8>
  %v21 = sea.nondet "nd_size_t" [5] : i64 loc("_4"("seahorn/lib/array_list_helper.c":26:24))
  %v22 = sea.const 8 : i64 loc("_5"("seahorn/lib/array_list_helper.c":26:9))
  %v23 = sea.ptr.add %v6, %v22 : !sea.ptr<64>, i64 loc("_5"("seahorn/lib/array_list_helper.c":26:9))
  sea.br ^bb1
^bb1:
  %v24 = sea.store %v1[%v23], %v21 inplace align 8 : !sea.mem<!sea.ptr<64>, i8>, i64 loc("seahorn/lib/array_list_helper.c":26:22)
  %v25 = sea.nondet "nd_size_t" [6] : i64 loc("_7"("seahorn/lib/array_list_helper.c":27:21))
  %v26 = sea.const 24 : i64 loc("_8"("seahorn/lib/array_list_helper.c":27:9))
  %v27 = sea.ptr.add %v6, %v26 : !sea.ptr<64>, i64 loc("_8"("seahorn/lib/array_list_helper.c":27:9))
  sea.br ^bb2
^bb2:
  %v28 = sea.store %v24[%v27], %v25 inplace align 8 : !sea.mem<!sea.ptr<64>, i8>, i64 loc("seahorn/lib/array_list_helper.c":27:19)
  %v29 = sea.nondet "nd_size_t" [7] : i64 loc("_10"("seahorn/lib/array_list_helper.c":28:18))
  %v30 = sea.const 16 : i64 loc("_11"("seahorn/lib/array_list_helper.c":28:9))
  %v31 = sea.ptr.add %v6, %v30 : !sea.ptr<64>, i64 loc("_11"("seahorn/lib/array_list_helper.c":28:9))
  sea.br ^bb3
^bb3:
  %v32 = sea.store %v28[%v31], %v29 inplace align 8 : !sea.mem<!sea.ptr<64>, i8>, i64 loc("seahorn/lib/array_list_helper.c":28:16)
  sea.br ^bb4
^bb4:
  %v33 = sea.const 0 : i64 loc("_14"("seahorn/lib/proof_allocators.c":21:15))
  %v34 = sea.icmp eq, %v21, %v33 : i64 loc("_14"("seahorn/lib/proof_allocators.c":21:15))
  %v35 = sea.ptr.null : !sea.ptr<64> loc("seahorn/lib/proof_allocators.c":21:10)
  sea.cond_br %v34, ^bb8(%v2, %v35 : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64>), ^bb5 loc("seahorn/lib/proof_allocators.c":21:10)
^bb5:
  %v36 = sea.nondet "nd_malloc_is_fail" [8] : i1 loc("_16"("seahorn/lib/sea_allocators.c":11:10))
  %v37 = sea.ptr.null : !sea.ptr<64> loc("seahorn/lib/sea_allocators.c":11:10)
  sea.cond_br %v36, ^bb7(%v2, %v37 : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64>), ^bb6 loc("seahorn/lib/sea_allocators.c":11:10)
^bb6:
  %v38 = sea.const 1 : i64 loc("malloc.i")
  %v39 = sea.mul %v21, %v38 : i64 loc("malloc.i")
  %v40 = sea.alloca %v39 align 16 [9] : i64 -> !sea.ptr<64> loc("malloc.i")
  sea.br ^bb7(%v2, %v40 : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64>) loc("seahorn/lib/sea_allocators.c":11:10)
^bb7(%v41 : !sea.mem<!sea.ptr<64>, i8>, %v42 : !sea.ptr<64>):
  %v43 = sea.memhavoc %v41[%v42], %v21 [10] inplace : !sea.mem<!sea.ptr<64>, i8> loc("seahorn/lib/sea_allocators.c":33:3)
  sea.br ^bb8(%v43, %v42 : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64>) loc("seahorn/lib/proof_allocators.c":21:10)
^bb8(%v44 : !sea.mem<!sea.ptr<64>, i8>, %v45 : !sea.ptr<64>):
  %v46 = sea.const 32 : i64 loc("_20"("seahorn/lib/array_list_helper.c":29:9))
  %v47 = sea.ptr.add %v6, %v46 : !sea.ptr<64>, i64 loc("_20"("seahorn/lib/array_list_helper.c":29:9))
  sea.br ^bb9
^bb9:
  %v48 = sea.store %v32[%v47], %v45 inplace align 8 : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64> loc("seahorn/lib/array_list_helper.c":29:14)
  sea.br ^bb10
^bb10:
  %v49 = sea.global @"s_allocator_static" size 40 align 8 : !sea.ptr<64> loc("seahorn/lib/array_list_helper.c":30:15)
  %v50 = sea.store %v48[%v6], %v49 inplace align 8 : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64> loc("seahorn/lib/array_list_helper.c":30:15)
  sea.br ^bb11
^bb11:
  sea.br ^bb12
^bb12:
  %v51 = sea.const 3 : i64 loc("_23"("seahorn/lib/array_list_helper.c":38:47))
  %v52 = sea.icmp ult, %v25, %v51 : i64 loc("_23"("seahorn/lib/array_list_helper.c":38:47))
  %v53 = sea.const 9223372036854775809 : i64 loc("_24"("seahorn/lib/array_list_helper.c":39:41))
  %v54 = sea.icmp ult, %v29, %v53 : i64 loc("_24"("seahorn/lib/array_list_helper.c":39:41))
  %v55 = sea.const 0 : i1 loc("_25"("seahorn/lib/array_list_helper.c":40:31))
  %v56 = sea.select %v52, %v54, %v55 : i1 loc("_25"("seahorn/lib/array_list_helper.c":40:31))
  %v57 = sea.const 1 : i1 loc("_26"("seahorn/lib/array_list_helper.c":31:3))
  %v58 = sea.and %v57, %v56 : i1 loc("_26"("seahorn/lib/array_list_helper.c":31:3))
  %v59 = sea.nondet "nd_size_t" [11] : i64 loc("_27"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":19:22))
  %v60 = sea.nondet "nd_size_t" [12] : i64 loc("_28"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":20:22))
  sea.br ^bb13
^bb13:
  sea.br ^bb14
^bb14:
  sea.br ^bb15
^bb15:
  %v61 = sea.mul %v29, %v25 : i64 loc("_32"("aws-c-common/include/aws/common/math.gcc_overflow.inl":35:9))
  %v62 = sea.umulo %v29, %v25 : i64 loc("_32"("aws-c-common/include/aws/common/math.gcc_overflow.inl":35:9))
  sea.cond_br %v62, ^bb16, ^bb17 loc("aws-c-common/include/aws/common/math.gcc_overflow.inl":35:9)
^bb16:
  sea.br ^bb17 loc("aws-c-common/include/aws/common/math.gcc_overflow.inl":36:9)
^bb17:
  %v63 = sea.const 1 : i1 loc("_36"("aws-c-common/include/aws/common/array_list.inl":92:41))
  %v64 = sea.xor %v62, %v63 : i1 loc("_36"("aws-c-common/include/aws/common/array_list.inl":92:41))
  sea.br ^bb18
^bb18:
  %v65 = sea.icmp uge, %v21, %v61 : i64 loc("_38"("aws-c-common/include/aws/common/array_list.inl":92:54))
  sea.br ^bb19
^bb19:
  sea.cond_br %v34, ^bb24, ^bb20 loc("aws-c-common/include/aws/common/array_list.inl":93:26)
^bb20:
  sea.br ^bb21
^bb21:
  sea.br ^bb22
^bb22:
  sea.br ^bb23
^bb23:
  %v66 = sea.ptr.null : !sea.ptr<64> loc("_46"("aws-c-common/include/aws/common/array_list.inl":94:26))
  %v67 = sea.ptr.cmp ne, %v45, %v66 : !sea.ptr<64> loc("_46"("aws-c-common/include/aws/common/array_list.inl":94:26))
  sea.br ^bb27(%v67 : i1) loc("aws-c-common/include/aws/common/array_list.inl":94:26)
^bb24:
  sea.br ^bb25
^bb25:
  %v68 = sea.ptr.null : !sea.ptr<64> loc("_42"("aws-c-common/include/aws/common/array_list.inl":93:26))
  %v69 = sea.ptr.cmp eq, %v45, %v68 : !sea.ptr<64> loc("_42"("aws-c-common/include/aws/common/array_list.inl":93:26))
  %v70 = sea.const 0 : i1 loc("aws-c-common/include/aws/common/array_list.inl":93:83)
  sea.cond_br %v69, ^bb26, ^bb27(%v70 : i1) loc("aws-c-common/include/aws/common/array_list.inl":93:83)
^bb26:
  %v71 = sea.const 1 : i1
  sea.br ^bb27(%v71 : i1)
^bb27(%v72 : i1):
  sea.br ^bb28
^bb28:
  %v73 = sea.const 0 : i1 loc("or.cond.i.i"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v74 = sea.select %v64, %v65, %v73 : i1 loc("or.cond.i.i"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v75 = sea.and %v74, %v72 : i1 loc("or.cond3.i.i"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v76 = sea.const 0 : i64 loc("_49")
  %v77 = sea.icmp ne, %v25, %v76 : i64 loc("_49")
  %v78 = sea.const 0 : i1 loc("spec.select"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v79 = sea.select %v75, %v77, %v78 : i1 loc("spec.select"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v80 = sea.and %v58, %v79 : i1 loc("_50"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":23:5))
  sea.br ^bb29
^bb29:
  %v81 = sea.const 0 : i64 loc(".not.i50.i"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v82 = sea.icmp eq, %v29, %v81 : i64 loc(".not.i50.i"("aws-c-common/include/aws/common/array_list.inl":341:5))
  sea.cond_br %v82, ^bb32(%v80 : i1), ^bb30 loc("aws-c-common/include/aws/common/array_list.inl":341:5)
^bb30:
  sea.br ^bb31
^bb31:
  %v83 = sea.ptr.null : !sea.ptr<64> loc(".not1.i51.i"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v84 = sea.ptr.cmp eq, %v45, %v83 : !sea.ptr<64> loc(".not1.i51.i"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v85 = sea.const 1 : i1 loc("_54"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v86 = sea.xor %v84, %v85 : i1 loc("_54"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v87 = sea.and %v80, %v86 : i1 loc("_55"("aws-c-common/include/aws/common/array_list.inl":341:5))
  sea.br ^bb32(%v87 : i1) loc("aws-c-common/include/aws/common/array_list.inl":341:5)
^bb32(%v88 : i1):
  sea.br ^bb33
^bb33:
  %v89 = sea.icmp ult, %v59, %v29 : i64 loc("_57"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":28:20))
  %v90 = sea.and %v88, %v89 : i1 loc("_58"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":28:5))
  sea.br ^bb34
^bb34:
  sea.cond_br %v82, ^bb37(%v90 : i1), ^bb35 loc("aws-c-common/include/aws/common/array_list.inl":341:5)
^bb35:
  sea.br ^bb36
^bb36:
  %v91 = sea.ptr.null : !sea.ptr<64> loc(".not1.i54.i"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v92 = sea.ptr.cmp eq, %v45, %v91 : !sea.ptr<64> loc(".not1.i54.i"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v93 = sea.const 1 : i1 loc("_62"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v94 = sea.xor %v92, %v93 : i1 loc("_62"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v95 = sea.and %v90, %v94 : i1 loc("_63"("aws-c-common/include/aws/common/array_list.inl":341:5))
  sea.br ^bb37(%v95 : i1) loc("aws-c-common/include/aws/common/array_list.inl":341:5)
^bb37(%v96 : i1):
  sea.br ^bb38
^bb38:
  %v97 = sea.icmp ult, %v60, %v29 : i64 loc("_65"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":29:20))
  %v98 = sea.and %v96, %v97 : i1 loc("_66"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":29:5))
  %v99 = sea.mul %v59, %v25 : i64 loc("_67"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":35:32))
  %v100 = sea.ptr.add %v45, %v99 : !sea.ptr<64>, i64 loc("_68"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":36:23))
  %v101 = sea.const 1 : i64 loc("_69"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":36:23))
  %v102 = sea.ptr.dereferenceable %v100, %v101 : !sea.ptr<64>, i64 loc("_69"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":36:23))
  sea.cond_br %v102, ^bb39, ^bb127(%v98 : i1)
^bb39:
  %v103 = sea.load %v44[%v100] : !sea.mem<!sea.ptr<64>, i8> -> i8 loc("_71"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":36:23))
  %v104 = sea.mul %v60, %v25 : i64 loc("_72"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":40:32))
  %v105 = sea.ptr.add %v45, %v104 : !sea.ptr<64>, i64 loc("_73"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":41:23))
  %v106 = sea.const 1 : i64 loc("_74"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":41:23))
  %v107 = sea.ptr.dereferenceable %v105, %v106 : !sea.ptr<64>, i64 loc("_74"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":41:23))
  sea.cond_br %v107, ^bb40, ^bb127(%v98 : i1)
^bb40:
  %v108 = sea.load %v44[%v105] : !sea.mem<!sea.ptr<64>, i8> -> i8 loc("_76"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":41:23))
  sea.br ^bb41
^bb41:
  %v109 = sea.and %v98, %v89 : i1 loc("_78"("aws-c-common/source/array_list.c":193:5))
  sea.br ^bb42
^bb42:
  %v110 = sea.and %v109, %v97 : i1 loc("_80"("aws-c-common/source/array_list.c":194:5))
  %v111 = sea.icmp eq, %v59, %v60 : i64 loc("_81"("aws-c-common/source/array_list.c":197:11))
  sea.cond_br %v111, ^bb83(%v110, %v44 : i1, !sea.mem<!sea.ptr<64>, i8>), ^bb43 loc("aws-c-common/source/array_list.c":197:9)
^bb43:
  sea.br ^bb44
^bb44:
  sea.cond_br %v82, ^bb47(%v110 : i1), ^bb45 loc("aws-c-common/include/aws/common/array_list.inl":341:5)
^bb45:
  sea.br ^bb46
^bb46:
  %v112 = sea.const 1 : i1 loc("_86"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v113 = sea.and %v110, %v112 : i1 loc("_86"("aws-c-common/include/aws/common/array_list.inl":341:5))
  sea.br ^bb47(%v113 : i1) loc("aws-c-common/include/aws/common/array_list.inl":341:5)
^bb47(%v114 : i1):
  sea.br ^bb48
^bb48:
  sea.cond_br %v89, ^bb50, ^bb49 loc("aws-c-common/include/aws/common/array_list.inl":367:9)
^bb49:
  %v115 = sea.ptr.null : !sea.ptr<64> loc("aws-c-common/include/aws/common/array_list.inl":373:5)
  sea.br ^bb54(%v115 : !sea.ptr<64>) loc("aws-c-common/include/aws/common/array_list.inl":373:5)
^bb50:
  sea.br ^bb51
^bb51:
  sea.br ^bb52
^bb52:
  sea.br ^bb53
^bb53:
  sea.br ^bb54(%v100 : !sea.ptr<64>) loc("aws-c-common/include/aws/common/array_list.inl":370:9)
^bb54(%v116 : !sea.ptr<64>):
  sea.br ^bb55
^bb55:
  sea.cond_br %v82, ^bb58(%v114 : i1), ^bb56 loc("aws-c-common/include/aws/common/array_list.inl":341:5)
^bb56:
  sea.br ^bb57
^bb57:
  %v117 = sea.const 1 : i1 loc("_97"("aws-c-common/include/aws/common/array_list.inl":341:5))
  %v118 = sea.and %v114, %v117 : i1 loc("_97"("aws-c-common/include/aws/common/array_list.inl":341:5))
  sea.br ^bb58(%v118 : i1) loc("aws-c-common/include/aws/common/array_list.inl":341:5)
^bb58(%v119 : i1):
  sea.br ^bb59
^bb59:
  sea.cond_br %v97, ^bb61, ^bb60 loc("aws-c-common/include/aws/common/array_list.inl":367:9)
^bb60:
  %v120 = sea.ptr.null : !sea.ptr<64> loc("aws-c-common/include/aws/common/array_list.inl":373:5)
  sea.br ^bb65(%v120 : !sea.ptr<64>) loc("aws-c-common/include/aws/common/array_list.inl":373:5)
^bb61:
  sea.br ^bb62
^bb62:
  sea.br ^bb63
^bb63:
  sea.br ^bb64
^bb64:
  sea.br ^bb65(%v105 : !sea.ptr<64>) loc("aws-c-common/include/aws/common/array_list.inl":370:9)
^bb65(%v121 : !sea.ptr<64>):
  sea.br ^bb66
^bb66:
  %v122 = sea.ptr.null : !sea.ptr<64> loc(".not.i.i"("aws-c-common/source/array_list.c":172:5))
  %v123 = sea.ptr.cmp eq, %v116, %v122 : !sea.ptr<64> loc(".not.i.i"("aws-c-common/source/array_list.c":172:5))
  %v124 = sea.const 1 : i1 loc("_106"("aws-c-common/source/array_list.c":172:5))
  %v125 = sea.xor %v123, %v124 : i1 loc("_106"("aws-c-common/source/array_list.c":172:5))
  %v126 = sea.and %v119, %v125 : i1 loc("_107"("aws-c-common/source/array_list.c":172:5))
  %v127 = sea.ptr.null : !sea.ptr<64> loc(".not3.i.i"("aws-c-common/source/array_list.c":173:5))
  %v128 = sea.ptr.cmp eq, %v121, %v127 : !sea.ptr<64> loc(".not3.i.i"("aws-c-common/source/array_list.c":173:5))
  %v129 = sea.const 1 : i1 loc("_108"("aws-c-common/source/array_list.c":173:5))
  %v130 = sea.xor %v128, %v129 : i1 loc("_108"("aws-c-common/source/array_list.c":173:5))
  %v131 = sea.and %v126, %v130 : i1 loc("_109"("aws-c-common/source/array_list.c":173:5))
  %v132 = sea.const 128 : i64 loc(".not"("aws-c-common/source/array_list.c":178:26))
  %v133 = sea.icmp ult, %v25, %v132 : i64 loc(".not"("aws-c-common/source/array_list.c":178:26))
  sea.cond_br %v133, ^bb76(%v44, %v121, %v116 : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64>, !sea.ptr<64>), ^bb67 loc("aws-c-common/source/array_list.c":178:5)
^bb67:
  sea.br ^bb68 loc("seahorn/lib/bcmp.c":161:3)
^bb68:
  %v134 = sea.const 128 : i64 loc("_111"("seahorn/lib/bcmp.c":162:3))
  %v135 = sea.ptr.dereferenceable %v116, %v134 : !sea.ptr<64>, i64 loc("_111"("seahorn/lib/bcmp.c":162:3))
  %v136 = sea.const 128 : i64 loc("_112"("seahorn/lib/bcmp.c":162:3))
  %v137 = sea.ptr.dereferenceable %v116, %v136 : !sea.ptr<64>, i64 loc("_112"("seahorn/lib/bcmp.c":162:3))
  sea.cond_br %v137, ^bb69, ^bb127(%v131 : i1) loc("seahorn/lib/bcmp.c":162:3)
^bb69:
  %v138 = sea.const 128 : i64 loc("seahorn/lib/bcmp.c":163:10)
  %v139 = sea.memmove %v44[%v4], %v44[%v116], %v138 inplace align 16 : !sea.mem<!sea.ptr<64>, i8> loc("seahorn/lib/bcmp.c":163:10)
  %v140 = sea.const 128 : i64 loc("_113"("seahorn/lib/bcmp.c":161:3))
  %v141 = sea.ptr.dereferenceable %v116, %v140 : !sea.ptr<64>, i64 loc("_113"("seahorn/lib/bcmp.c":161:3))
  %v142 = sea.const 128 : i64 loc("_114"("seahorn/lib/bcmp.c":161:3))
  %v143 = sea.ptr.dereferenceable %v116, %v142 : !sea.ptr<64>, i64 loc("_114"("seahorn/lib/bcmp.c":161:3))
  sea.cond_br %v143, ^bb70, ^bb127(%v131 : i1) loc("seahorn/lib/bcmp.c":161:3)
^bb70:
  %v144 = sea.const 128 : i64 loc("_116"("seahorn/lib/bcmp.c":162:3))
  %v145 = sea.ptr.dereferenceable %v121, %v144 : !sea.ptr<64>, i64 loc("_116"("seahorn/lib/bcmp.c":162:3))
  %v146 = sea.const 128 : i64 loc("_117"("seahorn/lib/bcmp.c":162:3))
  %v147 = sea.ptr.dereferenceable %v121, %v146 : !sea.ptr<64>, i64 loc("_117"("seahorn/lib/bcmp.c":162:3))
  sea.cond_br %v147, ^bb71, ^bb127(%v131 : i1) loc("seahorn/lib/bcmp.c":162:3)
^bb71:
  %v148 = sea.const 128 : i64 loc("seahorn/lib/bcmp.c":163:10)
  %v149 = sea.memmove %v139[%v116], %v139[%v121], %v148 inplace : !sea.mem<!sea.ptr<64>, i8> loc("seahorn/lib/bcmp.c":163:10)
  %v150 = sea.const 128 : i64 loc("_118"("seahorn/lib/bcmp.c":161:3))
  %v151 = sea.ptr.dereferenceable %v121, %v150 : !sea.ptr<64>, i64 loc("_118"("seahorn/lib/bcmp.c":161:3))
  %v152 = sea.const 128 : i64 loc("_119"("seahorn/lib/bcmp.c":161:3))
  %v153 = sea.ptr.dereferenceable %v121, %v152 : !sea.ptr<64>, i64 loc("_119"("seahorn/lib/bcmp.c":161:3))
  sea.cond_br %v153, ^bb72, ^bb127(%v131 : i1) loc("seahorn/lib/bcmp.c":161:3)
^bb72:
  sea.br ^bb73 loc("seahorn/lib/bcmp.c":162:3)
^bb73:
  %v154 = sea.const 128 : i64 loc("seahorn/lib/bcmp.c":163:10)
  %v155 = sea.memmove %v149[%v121], %v149[%v4], %v154 inplace src_align 16 : !sea.mem<!sea.ptr<64>, i8> loc("seahorn/lib/bcmp.c":163:10)
  %v156 = sea.const 18446744073709551488 : i64 loc(".mask"("aws-c-common/source/array_list.c":178:26))
  %v157 = sea.and %v25, %v156 : i64 loc(".mask"("aws-c-common/source/array_list.c":178:26))
  %v158 = sea.const 128 : i64 loc("exitcond.not"("aws-c-common/source/array_list.c":178:26))
  %v159 = sea.icmp eq, %v157, %v158 : i64 loc("exitcond.not"("aws-c-common/source/array_list.c":178:26))
  sea.cond_br %v159, ^bb75, ^bb74 loc("aws-c-common/source/array_list.c":178:5)
^bb74:
  %v160 = sea.const 0 : i1 loc("_110")
  %v161 = sea.and %v131, %v160 : i1 loc("_110")
  sea.br ^bb127(%v161 : i1) loc("seahorn/lib/bcmp.c":161:3)
^bb75:
  %v162 = sea.const 128 : i64 loc("_121"("aws-c-common/source/array_list.c":183:34))
  %v163 = sea.ptr.add %v121, %v162 : !sea.ptr<64>, i64 loc("_121"("aws-c-common/source/array_list.c":183:34))
  %v164 = sea.const 128 : i64 loc("_122"("aws-c-common/source/array_list.c":182:34))
  %v165 = sea.ptr.add %v116, %v164 : !sea.ptr<64>, i64 loc("_122"("aws-c-common/source/array_list.c":182:34))
  sea.br ^bb76(%v155, %v163, %v165 : !sea.mem<!sea.ptr<64>, i8>, !sea.ptr<64>, !sea.ptr<64>) loc("aws-c-common/source/array_list.c":186:34)
^bb76(%v166 : !sea.mem<!sea.ptr<64>, i8>, %v167 : !sea.ptr<64>, %v168 : !sea.ptr<64>):
  %v169 = sea.const 127 : i64 loc("_123"("aws-c-common/source/array_list.c":186:34))
  %v170 = sea.and %v25, %v169 : i64 loc("_123"("aws-c-common/source/array_list.c":186:34))
  %v171 = sea.ptr.dereferenceable %v4, %v170 : !sea.ptr<64>, i64 loc("_124"("seahorn/lib/bcmp.c":161:3))
  %v172 = sea.ptr.dereferenceable %v4, %v170 : !sea.ptr<64>, i64 loc("_125"("seahorn/lib/bcmp.c":161:3))
  sea.cond_br %v172, ^bb77, ^bb127(%v131 : i1) loc("seahorn/lib/bcmp.c":161:3)
^bb77:
  %v173 = sea.ptr.dereferenceable %v168, %v170 : !sea.ptr<64>, i64 loc("_127"("seahorn/lib/bcmp.c":162:3))
  %v174 = sea.ptr.dereferenceable %v168, %v170 : !sea.ptr<64>, i64 loc("_128"("seahorn/lib/bcmp.c":162:3))
  sea.cond_br %v174, ^bb78, ^bb127(%v131 : i1) loc("seahorn/lib/bcmp.c":162:3)
^bb78:
  %v175 = sea.memmove %v166[%v4], %v166[%v168], %v170 inplace align 16 : !sea.mem<!sea.ptr<64>, i8> loc("seahorn/lib/bcmp.c":163:10)
  %v176 = sea.ptr.dereferenceable %v168, %v170 : !sea.ptr<64>, i64 loc("_129"("seahorn/lib/bcmp.c":161:3))
  %v177 = sea.ptr.dereferenceable %v168, %v170 : !sea.ptr<64>, i64 loc("_130"("seahorn/lib/bcmp.c":161:3))
  sea.cond_br %v177, ^bb79, ^bb127(%v131 : i1) loc("seahorn/lib/bcmp.c":161:3)
^bb79:
  %v178 = sea.ptr.dereferenceable %v167, %v170 : !sea.ptr<64>, i64 loc("_132"("seahorn/lib/bcmp.c":162:3))
  %v179 = sea.ptr.dereferenceable %v167, %v170 : !sea.ptr<64>, i64 loc("_133"("seahorn/lib/bcmp.c":162:3))
  sea.cond_br %v179, ^bb80, ^bb127(%v131 : i1) loc("seahorn/lib/bcmp.c":162:3)
^bb80:
  %v180 = sea.memmove %v175[%v168], %v175[%v167], %v170 inplace : !sea.mem<!sea.ptr<64>, i8> loc("seahorn/lib/bcmp.c":163:10)
  %v181 = sea.ptr.dereferenceable %v167, %v170 : !sea.ptr<64>, i64 loc("_134"("seahorn/lib/bcmp.c":161:3))
  %v182 = sea.ptr.dereferenceable %v167, %v170 : !sea.ptr<64>, i64 loc("_135"("seahorn/lib/bcmp.c":161:3))
  sea.cond_br %v182, ^bb81, ^bb127(%v131 : i1) loc("seahorn/lib/bcmp.c":161:3)
^bb81:
  %v183 = sea.ptr.dereferenceable %v4, %v170 : !sea.ptr<64>, i64 loc("_137"("seahorn/lib/bcmp.c":162:3))
  %v184 = sea.ptr.dereferenceable %v4, %v170 : !sea.ptr<64>, i64 loc("_138"("seahorn/lib/bcmp.c":162:3))
  sea.cond_br %v184, ^bb82, ^bb127(%v131 : i1) loc("seahorn/lib/bcmp.c":162:3)
^bb82:
  %v185 = sea.memmove %v180[%v167], %v180[%v4], %v170 inplace src_align 16 : !sea.mem<!sea.ptr<64>, i8> loc("seahorn/lib/bcmp.c":163:10)
  sea.br ^bb83(%v131, %v185 : i1, !sea.mem<!sea.ptr<64>, i8>)
^bb83(%v186 : i1, %v187 : !sea.mem<!sea.ptr<64>, i8>):
  sea.br ^bb84
^bb84:
  %v188 = sea.load %v32[%v31] align 8 : !sea.mem<!sea.ptr<64>, i8> -> i64 loc("_140"("aws-c-common/include/aws/common/array_list.inl":91:37))
  sea.br ^bb85
^bb85:
  %v189 = sea.load %v28[%v27] align 8 : !sea.mem<!sea.ptr<64>, i8> -> i64 loc("_142"("aws-c-common/include/aws/common/array_list.inl":91:51))
  sea.br ^bb86
^bb86:
  %v190 = sea.mul %v188, %v189 : i64 loc("_144"("aws-c-common/include/aws/common/math.gcc_overflow.inl":35:9))
  %v191 = sea.umulo %v188, %v189 : i64 loc("_144"("aws-c-common/include/aws/common/math.gcc_overflow.inl":35:9))
  sea.cond_br %v191, ^bb87, ^bb88 loc("aws-c-common/include/aws/common/math.gcc_overflow.inl":35:9)
^bb87:
  sea.br ^bb88 loc("aws-c-common/include/aws/common/math.gcc_overflow.inl":36:9)
^bb88:
  %v192 = sea.const 1 : i1 loc("_148"("aws-c-common/include/aws/common/array_list.inl":92:41))
  %v193 = sea.xor %v191, %v192 : i1 loc("_148"("aws-c-common/include/aws/common/array_list.inl":92:41))
  sea.br ^bb89
^bb89:
  %v194 = sea.load %v24[%v23] align 8 : !sea.mem<!sea.ptr<64>, i8> -> i64 loc("_150"("aws-c-common/include/aws/common/array_list.inl":92:41))
  %v195 = sea.icmp uge, %v194, %v190 : i64 loc("_151"("aws-c-common/include/aws/common/array_list.inl":92:54))
  sea.br ^bb90
^bb90:
  %v196 = sea.const 0 : i64 loc("_153"("aws-c-common/include/aws/common/array_list.inl":93:26))
  %v197 = sea.icmp eq, %v194, %v196 : i64 loc("_153"("aws-c-common/include/aws/common/array_list.inl":93:26))
  sea.cond_br %v197, ^bb95, ^bb91 loc("aws-c-common/include/aws/common/array_list.inl":93:26)
^bb91:
  sea.br ^bb92
^bb92:
  sea.br ^bb93
^bb93:
  sea.br ^bb94
^bb94:
  %v198 = sea.load %v48[%v47] align 8 : !sea.mem<!sea.ptr<64>, i8> -> !sea.ptr<64> loc("_161"("aws-c-common/include/aws/common/array_list.inl":94:26))
  %v199 = sea.ptr.null : !sea.ptr<64> loc("_162"("aws-c-common/include/aws/common/array_list.inl":94:26))
  %v200 = sea.ptr.cmp ne, %v198, %v199 : !sea.ptr<64> loc("_162"("aws-c-common/include/aws/common/array_list.inl":94:26))
  sea.br ^bb98(%v198, %v200 : !sea.ptr<64>, i1) loc("aws-c-common/include/aws/common/array_list.inl":94:26)
^bb95:
  sea.br ^bb96
^bb96:
  %v201 = sea.load %v48[%v47] align 8 : !sea.mem<!sea.ptr<64>, i8> -> !sea.ptr<64> loc("_156"("aws-c-common/include/aws/common/array_list.inl":93:26))
  %v202 = sea.ptr.null : !sea.ptr<64> loc("_157"("aws-c-common/include/aws/common/array_list.inl":93:26))
  %v203 = sea.ptr.cmp eq, %v201, %v202 : !sea.ptr<64> loc("_157"("aws-c-common/include/aws/common/array_list.inl":93:26))
  %v204 = sea.const 0 : i1 loc("aws-c-common/include/aws/common/array_list.inl":93:83)
  sea.cond_br %v203, ^bb97, ^bb98(%v201, %v204 : !sea.ptr<64>, i1) loc("aws-c-common/include/aws/common/array_list.inl":93:83)
^bb97:
  %v205 = sea.ptr.null : !sea.ptr<64>
  %v206 = sea.const 1 : i1
  sea.br ^bb98(%v205, %v206 : !sea.ptr<64>, i1)
^bb98(%v207 : !sea.ptr<64>, %v208 : i1):
  sea.br ^bb99
^bb99:
  %v209 = sea.const 0 : i1 loc("or.cond.i39.i"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v210 = sea.select %v193, %v195, %v209 : i1 loc("or.cond.i39.i"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v211 = sea.const 0 : i1 loc("or.cond3.i40.i"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v212 = sea.select %v210, %v208, %v211 : i1 loc("or.cond3.i40.i"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v213 = sea.const 0 : i64 loc("_166")
  %v214 = sea.icmp ne, %v189, %v213 : i64 loc("_166")
  %v215 = sea.const 0 : i1 loc("spec.select8"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v216 = sea.select %v212, %v214, %v215 : i1 loc("spec.select8"("aws-c-common/include/aws/common/array_list.inl":96:35))
  sea.br ^bb100
^bb100:
  sea.br ^bb101
^bb101:
  sea.br ^bb102
^bb102:
  sea.cond_br %v191, ^bb103, ^bb104 loc("aws-c-common/include/aws/common/math.gcc_overflow.inl":35:9)
^bb103:
  sea.br ^bb104 loc("aws-c-common/include/aws/common/math.gcc_overflow.inl":36:9)
^bb104:
  sea.br ^bb105
^bb105:
  sea.br ^bb106
^bb106:
  sea.cond_br %v197, ^bb111, ^bb107 loc("aws-c-common/include/aws/common/array_list.inl":93:26)
^bb107:
  sea.br ^bb108
^bb108:
  sea.br ^bb109
^bb109:
  sea.br ^bb110
^bb110:
  %v217 = sea.ptr.null : !sea.ptr<64> loc("_179"("aws-c-common/include/aws/common/array_list.inl":94:26))
  %v218 = sea.ptr.cmp ne, %v207, %v217 : !sea.ptr<64> loc("_179"("aws-c-common/include/aws/common/array_list.inl":94:26))
  sea.br ^bb114(%v218 : i1) loc("aws-c-common/include/aws/common/array_list.inl":94:26)
^bb111:
  sea.br ^bb112
^bb112:
  %v219 = sea.ptr.null : !sea.ptr<64> loc("_175"("aws-c-common/include/aws/common/array_list.inl":93:26))
  %v220 = sea.ptr.cmp eq, %v207, %v219 : !sea.ptr<64> loc("_175"("aws-c-common/include/aws/common/array_list.inl":93:26))
  %v221 = sea.const 0 : i1 loc("aws-c-common/include/aws/common/array_list.inl":93:83)
  sea.cond_br %v220, ^bb113, ^bb114(%v221 : i1) loc("aws-c-common/include/aws/common/array_list.inl":93:83)
^bb113:
  %v222 = sea.const 1 : i1
  sea.br ^bb114(%v222 : i1)
^bb114(%v223 : i1):
  sea.br ^bb115
^bb115:
  %v224 = sea.const 0 : i1 loc("or.cond3.i46.i"("aws-c-common/include/aws/common/array_list.inl":96:35))
  %v225 = sea.select %v210, %v223, %v224 : i1 loc("or.cond3.i46.i"("aws-c-common/include/aws/common/array_list.inl":96:35))
  sea.cond_br %v225, ^bb117, ^bb116 loc("aws-c-common/include/aws/common/array_list.inl":96:35)
^bb116:
  sea.br ^bb127(%v186 : i1) loc("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":47:5)
^bb117:
  %v226 = sea.const 0 : i64 loc(".not10"("aws-c-common/include/aws/common/array_list.inl":95:48))
  %v227 = sea.icmp eq, %v189, %v226 : i64 loc(".not10"("aws-c-common/include/aws/common/array_list.inl":95:48))
  sea.cond_br %v227, ^bb127(%v186 : i1), ^bb118 loc("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":47:5)
^bb118:
  %v228 = sea.load %v50[%v6] align 8 : !sea.mem<!sea.ptr<64>, i8> -> !sea.ptr<64> loc("_184"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":48:5))
  %v229 = sea.global @"s_allocator_static" size 40 align 8 : !sea.ptr<64> loc("_185"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":48:5))
  %v230 = sea.ptr.cmp eq, %v228, %v229 : !sea.ptr<64> loc("_185"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":48:5))
  sea.cond_br %v230, ^bb119, ^bb127(%v186 : i1) loc("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":48:5)
^bb119:
  %v231 = sea.icmp eq, %v194, %v21 : i64 loc("_187"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":49:5))
  sea.cond_br %v231, ^bb120, ^bb127(%v186 : i1) loc("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":49:5)
^bb120:
  %v232 = sea.icmp eq, %v188, %v29 : i64 loc("_189"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":50:5))
  sea.cond_br %v232, ^bb121, ^bb127(%v186 : i1) loc("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":50:5)
^bb121:
  %v233 = sea.icmp eq, %v189, %v25 : i64 loc("_191"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":51:5))
  sea.cond_br %v233, ^bb122, ^bb127(%v186 : i1) loc("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":51:5)
^bb122:
  %v234 = sea.ptr.add %v207, %v99 : !sea.ptr<64>, i64 loc("_193"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":54:5))
  %v235 = sea.const 1 : i64 loc("_194"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":54:5))
  %v236 = sea.ptr.dereferenceable %v234, %v235 : !sea.ptr<64>, i64 loc("_194"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":54:5))
  sea.cond_br %v236, ^bb123, ^bb127(%v186 : i1)
^bb123:
  %v237 = sea.load %v187[%v234] : !sea.mem<!sea.ptr<64>, i8> -> i8 loc("_196"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":54:5))
  %v238 = sea.icmp eq, %v237, %v108 : i8 loc("_197"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":54:5))
  %v239 = sea.const 1 : i64 loc("_198"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":54:5))
  %v240 = sea.ptr.dereferenceable %v234, %v239 : !sea.ptr<64>, i64 loc("_198"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":54:5))
  %v241 = sea.const 0 : i1 loc("_199")
  %v242 = sea.select %v240, %v238, %v241 : i1 loc("_199")
  sea.cond_br %v242, ^bb124, ^bb127(%v186 : i1)
^bb124:
  %v243 = sea.ptr.add %v207, %v104 : !sea.ptr<64>, i64 loc("_201"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5))
  %v244 = sea.const 1 : i64 loc("_202"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5))
  %v245 = sea.ptr.dereferenceable %v243, %v244 : !sea.ptr<64>, i64 loc("_202"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5))
  sea.cond_br %v245, ^bb125, ^bb127(%v186 : i1)
^bb125:
  %v246 = sea.load %v187[%v243] : !sea.mem<!sea.ptr<64>, i8> -> i8 loc("_204"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5))
  %v247 = sea.icmp eq, %v246, %v103 : i8 loc("_205"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5))
  %v248 = sea.const 1 : i64 loc("_206"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5))
  %v249 = sea.ptr.dereferenceable %v243, %v248 : !sea.ptr<64>, i64 loc("_206"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5))
  sea.cond_br %v249, ^bb126, ^bb127(%v186 : i1)
^bb126:
  %v250 = sea.const 1 : i1 loc("_208"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5))
  %v251 = sea.xor %v247, %v250 : i1 loc("_208"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5))
  %v252 = sea.and %v186, %v251 : i1 loc("_209"("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5))
  sea.br ^bb127(%v252 : i1) loc("seahorn/jobs/array_list_swap/aws_array_list_swap_harness.c":55:5)
^bb127(%v253 : i1):
  sea.assume %v253
  sea.br ^bb128
^bb128:
  %v254 = sea.const 0 : i1
  sea.assert %v254
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
