// RUN: sea-opt %s --sea-expand-mem-fill | FileCheck %s
// keep-bytes (sea-vc --seam): the fill of a byte memory stays (SeaM's mfill
// term), that of a word memory is expanded and counted.
// RUN: sea-opt %s --sea-expand-mem-fill=keep-bytes=true | FileCheck %s --check-prefix=KEEP
// RUN: sea-opt %s --sea-widen-memory="k=8 report=true" --sea-expand-mem-fill 2>/dev/null | FileCheck %s --check-prefix=WIDE
// RUN: sea-opt %s --sea-widen-memory="k=8 report=true" -o /dev/null 2>&1 | FileCheck %s --check-prefix=REM
// RUN: sea-opt %s --sea-widen-memory="k=8 report=true" 2>/dev/null | FileCheck %s --check-prefix=PACK
// The expansion writes what the fill writes: every program runs the same.
// RUN: sea-opt %s --sea-expand-mem-fill | sea-run - --program=bytes --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-widen-memory=k=8 --sea-expand-mem-fill 2>/dev/null | sea-run - --program=widened --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-widen-memory=k=8 --sea-expand-mem-fill 2>/dev/null | sea-run - --program=unaligned_start --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-widen-memory=k=8 --sea-expand-mem-fill 2>/dev/null | sea-run - --program=unaligned_end --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-expand-mem-fill | sea-run - --program=words --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-widen-memory="k=8 report=true" -o /dev/null 2>&1 | FileCheck %s --check-prefix=CLS
// RUN: sea-opt %s --sea-widen-memory=k=8 2>/dev/null | FileCheck %s --check-prefix=CLSPACK
// RUN: sea-opt %s --sea-widen-memory=k=8 --sea-expand-mem-fill 2>/dev/null | sea-run - --program=percls --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-widen-memory=k=8 --sea-expand-mem-fill 2>/dev/null | sea-run - --program=join --missing=zero | FileCheck %s --check-prefix=OK

// sea-expand-mem-fill: one store of the memory's cell type per cell, at
// addr + k * (units per cell), in order; no fill left. sea-widen-memory
// widens a memory with a fill only if the fill covers whole aligned words,
// and then packs its bytes into words (little-endian); a fill that starts
// or ends inside a word keeps the memory bytes, as a narrow store does.

// OK: status: halted

!m = !sea.mem<!sea.ptr<64>, i8>
!w = !sea.mem<!sea.ptr<64>, i64, aligned>

// Cell width 1: three i8 stores at +0, +1, +2; the first consumes the
// fill's memory, inplace as the fill; tracked kept.
// CHECK-LABEL: sea.program @bytes
// CHECK-NOT: sea.mem.fill
// CHECK: %[[S0:.*]] = sea.store %{{.*}}[%[[G:.*]]], %{{.*}} inplace tracked : !sea.mem<!sea.ptr<64>, i8>, i8
// CHECK: %[[A1:.*]] = sea.ptr.add %[[G]], %{{.*}} : !sea.ptr<64>, i64
// CHECK: %[[S1:.*]] = sea.store %[[S0]][%[[A1]]], %{{.*}} inplace tracked : !sea.mem<!sea.ptr<64>, i8>, i8
// CHECK: %[[A2:.*]] = sea.ptr.add %[[G]], %{{.*}} : !sea.ptr<64>, i64
// CHECK: sea.store %[[S1]][%[[A2]]], %{{.*}} inplace tracked : !sea.mem<!sea.ptr<64>, i8>, i8
// CHECK-NOT: sea.store
// CHECK: sea.load
// KEEP-LABEL: sea.program @bytes
// KEEP-SAME: sea.expand_mem_fill = {cells = 0 : i64, expanded = 0 : i64, "expanded-keyed" = 0 : i64, "expanded-other" = 0 : i64, "expanded-word-layout" = 0 : i64, kept = 1 : i64}
// KEEP: sea.mem.fill %{{.*}}, dense<[1, 2, 3]> : tensor<3xi8> inplace tracked
// KEEP-NOT: sea.store
sea.program @bytes {
  %m0 = sea.mem.nondet "m" [0] : !m
  %g = sea.global @tab size 4 align 4 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3]> : tensor<3xi8> inplace tracked : !m
  %x = sea.load %m1[%g] : !m -> i16
  %e = sea.const 513 : i16
  %ok = sea.icmp eq, %x, %e : i16
  sea.assert %ok
  sea.halt
}

// Cell width 8 (a word memory): two i64 stores at +0 and +8.
// CHECK-LABEL: sea.program @words
// CHECK-NOT: sea.mem.fill
// CHECK: sea.const 1 : i64
// CHECK: sea.store %{{.*}}{{\[}}[[W:%[0-9]+]]{{\]}}, %{{.*}} inplace : !sea.mem<!sea.ptr<64>, i64, aligned>, i64
// CHECK: %[[C8:.*]] = sea.const 8 : i64
// CHECK: %[[W8:.*]] = sea.ptr.add [[W]], %[[C8]]
// CHECK: sea.const -1 : i64
// CHECK: sea.store %{{.*}}[%[[W8]]], %{{.*}} inplace : !sea.mem<!sea.ptr<64>, i64, aligned>, i64
// CHECK-NOT: sea.store
// CHECK: sea.load
// KEEP-LABEL: sea.program @words
// KEEP-SAME: sea.expand_mem_fill = {cells = 2 : i64, expanded = 1 : i64, "expanded-keyed" = 0 : i64, "expanded-other" = 0 : i64, "expanded-word-layout" = 1 : i64, kept = 0 : i64}
// KEEP-NOT: sea.mem.fill
// KEEP-COUNT-2: sea.store
sea.program @words {
  %m0 = sea.mem.nondet "m" [0] : !w
  %g = sea.global @wtab size 16 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, -1]> : tensor<2xi64> inplace : !w
  %c8 = sea.const 8 : i64
  %q = sea.ptr.add %g, %c8 : !sea.ptr<64>, i64
  %x = sea.load %m1[%q] : !w -> i64
  %e = sea.const -1 : i64
  %ok = sea.icmp eq, %x, %e : i64
  sea.assert %ok
  sea.halt
}

// 16 bytes at an 8-aligned global, only 8-byte loads: widened to words;
// the fill's data packed into two words, then one i64 store per word.
// REM-DAG: remark: {{.*}}memory widened to 8-byte words
// PACK-LABEL: sea.program @widened
// PACK: sea.mem.fill %{{.*}}, dense<[578437695752307201, 1157159078456920585]> : tensor<2xi64> inplace : !sea.mem<!sea.ptr<64>, i64, aligned>
// WIDE-LABEL: sea.program @widened
// WIDE-NOT: sea.mem.fill
// WIDE-COUNT-2: sea.store {{.*}} : !sea.mem<!sea.ptr<64>, i64, aligned>, i64
// WIDE-NOT: sea.store
sea.program @widened {
  %m0 = sea.mem.nondet "m" [1] : !m
  %g = sea.global @wide size 16 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]> : tensor<16xi8> inplace : !m
  %c8 = sea.const 8 : i64
  %q = sea.ptr.add %g, %c8 : !sea.ptr<64>, i64
  %x = sea.load %m1[%q] : !m -> i64
  %e = sea.const 1157159078456920585 : i64
  %ok = sea.icmp eq, %x, %e : i64
  sea.assert %ok
  sea.halt
}

// The fill starts inside a word (offset 3): the memory stays bytes, the
// fill becomes byte stores.
// REM-DAG: remark: {{.*}}memory kept as bytes: a fill not provably word-aligned
// WIDE-LABEL: sea.program @unaligned_start
// WIDE-NOT: sea.mem.fill
// WIDE-COUNT-8: sea.store {{.*}} : !sea.mem<!sea.ptr<64>, i8>, i8
// WIDE-NOT: sea.store
sea.program @unaligned_start {
  %m0 = sea.mem.nondet "m" [2] : !m
  %g = sea.global @ustart size 16 align 8 : !sea.ptr<64>
  %c3 = sea.const 3 : i64
  %p = sea.ptr.add %g, %c3 : !sea.ptr<64>, i64
  %m1 = sea.mem.fill %m0[%p], dense<[1, 2, 3, 4, 5, 6, 7, 8]> : tensor<8xi8> inplace : !m
  %c8 = sea.const 8 : i64
  %q = sea.ptr.add %g, %c8 : !sea.ptr<64>, i64
  %x = sea.load %m1[%q] : !m -> i64
  // bytes 8..15 = 6, 7, 8, then the zero contents
  %e = sea.const 526086 : i64
  %ok = sea.icmp eq, %x, %e : i64
  sea.assert %ok
  sea.halt
}

// The fill ends inside a word (13 bytes, odd: no word size fits): the
// memory stays bytes.
// REM-DAG: remark: {{.*}}memory kept as bytes: a fill length not a multiple of a word
// WIDE-LABEL: sea.program @unaligned_end
// WIDE-NOT: sea.mem.fill
// WIDE-COUNT-13: sea.store {{.*}} : !sea.mem<!sea.ptr<64>, i8>, i8
// WIDE-NOT: sea.store
// WIDE: sea.load
sea.program @unaligned_end {
  %m0 = sea.mem.nondet "m" [3] : !m
  %g = sea.global @uend size 16 align 8 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13]> : tensor<13xi8> inplace : !m
  %c8 = sea.const 8 : i64
  %q = sea.ptr.add %g, %c8 : !sea.ptr<64>, i64
  %x = sea.load %m1[%q] : !m -> i64
  // bytes 8..15 = 9, 10, 11, 12, 13, then the zero contents
  %e = sea.const 56036624905 : i64
  %ok = sea.icmp eq, %x, %e : i64
  sea.assert %ok
  sea.halt
}

// Widening is per class: the 12-byte fill is not a multiple of 8, so the
// align 4 global gets 4-byte words.
// CLS: remark: sea-widen-memory: memory widened to 4-byte words (not 8: a fill length not a multiple of a word
// CLSPACK-LABEL: sea.program @percls
// CLSPACK: dense<[67305985, 134678021, 202050057]> : tensor<3xi32>
sea.program @percls {
  %m0 = sea.mem.nondet "m" [0] : !m
  %g = sea.global @t size 16 align 4 : !sea.ptr<64>
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]> : tensor<12xi8> inplace : !m
  %i = sea.nondet "i" [1] : i64
  %c2 = sea.const 2 : i64
  %j = sea.and %i, %c2 : i64
  %c2b = sea.const 2 : i64
  %off = sea.mul %j, %c2b : i64
  %q = sea.ptr.add %g, %off : !sea.ptr<64>, i64
  %x = sea.load %m1[%q] align 4 : !m -> i32
  %e = sea.const 67305985 : i32
  %ok = sea.icmp eq, %x, %e : i32
  sea.assert %ok
  sea.halt
}

// Two fills on two branches reach one memory block argument: widened to 8.
sea.program @join {
  %m0 = sea.mem.nondet "m" [0] : !m
  %g = sea.global @t size 16 align 8 : !sea.ptr<64>
  %c = sea.nondet "c" [1] : i1
  sea.cond_br %c, ^a, ^b
^a:
  %m1 = sea.mem.fill %m0[%g], dense<[1, 2, 3, 4, 5, 6, 7, 8]> : tensor<8xi8> inplace : !m
  sea.br ^j(%m1 : !m)
^b:
  %c8 = sea.const 8 : i64
  %q = sea.ptr.add %g, %c8 : !sea.ptr<64>, i64
  %m2 = sea.mem.fill %m0[%q], dense<[-1, -2, -3, -4, -5, -6, -7, -8]> : tensor<8xi8> inplace : !m
  sea.br ^j(%m2 : !m)
^j(%m : !m):
  %x = sea.load %m[%g] align 8 : !m -> i64
  %c8b = sea.const 8 : i64
  %q2 = sea.ptr.add %g, %c8b : !sea.ptr<64>, i64
  %y = sea.load %m[%q2] align 8 : !m -> i64
  %s = sea.add %x, %y : i64
  %e = sea.const 578437695752307201 : i64
  %ok = sea.icmp ne, %s, %e : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
