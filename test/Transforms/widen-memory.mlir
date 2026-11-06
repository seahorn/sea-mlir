// RUN: sea-opt %s --sea-widen-memory="k=8 report=true" 2>/dev/null | FileCheck %s
// RUN: sea-opt %s --sea-widen-memory="k=8 report=true" -o /dev/null 2>&1 | FileCheck %s --check-prefix=REM

!m = !sea.mem<!sea.ptr<64>, i8>

// All accesses whole, aligned 8-byte words: widened (structural alignment:
// an alloca aligned to 8 plus constant offsets that are multiples of 8).
// REM-DAG: remark: {{.*}}memory widened to 8-byte words
// CHECK-LABEL: sea.program @widen
// CHECK: sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i64, aligned>
// CHECK: sea.memset {{.*}} : !sea.mem<!sea.ptr<64>, i64, aligned>
// CHECK: ^bb1(%{{.*}}: !sea.mem<!sea.ptr<64>, i64, aligned>):
sea.program @widen {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 32 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %eight = sea.const 8 : i64
  %q = sea.ptr.add %p, %eight : !sea.ptr<64>, i64
  %v = sea.const 7 : i64
  %m1 = sea.store %m0[%q], %v inplace : !m, i64
  %z = sea.const 0 : i8
  %sixteen = sea.const 16 : i64
  %m2 = sea.memset %m1[%p], %z, %sixteen inplace : !m
  sea.br ^bb1(%m2 : !m)
^bb1(%m3: !m):
  %x = sea.load %m3[%q] : !m -> i64
  sea.halt
}

// The op's `align` (LLVM's promise) is enough.
// CHECK-LABEL: sea.program @by_align
// CHECK: !sea.mem<!sea.ptr<64>, i64, aligned>
sea.program @by_align {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %x = sea.load %m0[%p] align 8 : !m -> i64
  sea.halt
}

// REM-DAG: remark: {{.*}}memory kept as bytes: a load narrower than (or not a multiple of) a word
// CHECK-LABEL: sea.program @narrow
// CHECK: sea.mem.nondet "m" [0] : !sea.mem<!sea.ptr<64>, i8>
sea.program @narrow {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %x = sea.load %m0[%p] : !m -> i32
  sea.halt
}

// REM-DAG: remark: {{.*}}memory kept as bytes: a store not provably word-aligned
// CHECK-LABEL: sea.program @misaligned
// CHECK: !sea.mem<!sea.ptr<64>, i8>
sea.program @misaligned {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %four = sea.const 4 : i64
  %q = sea.ptr.add %p, %four : !sea.ptr<64>, i64
  %v = sea.const 7 : i64
  %m1 = sea.store %m0[%q], %v inplace : !m, i64
  sea.halt
}

// REM-DAG: remark: {{.*}}memory kept as bytes: a memset length not a constant multiple of a word
// CHECK-LABEL: sea.program @symbolic_len
sea.program @symbolic_len {
  %m0 = sea.mem.nondet "m" [0] : !m
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [1] : i64 -> !sea.ptr<64>
  %len = sea.nondet "len" [2] : i64
  %z = sea.const 0 : i8
  %m1 = sea.memset %m0[%p], %z, %len inplace : !m
  sea.halt
}

// A memmove joins its source and destination: one narrow access in either
// keeps both as bytes.
// REM-DAG: remark: {{.*}}memory kept as bytes: a load narrower
// CHECK-LABEL: sea.program @joined
// CHECK: sea.mem.nondet "a" [0] : !sea.mem<!sea.ptr<64>, i8>
// CHECK: sea.mem.nondet "b" [1] : !sea.mem<!sea.ptr<64>, i8>
sea.program @joined {
  %a = sea.mem.nondet "a" [0] : !m
  %b = sea.mem.nondet "b" [1] : !m
  %n = sea.const 16 : i64
  %p = sea.alloca %n align 8 [2] : i64 -> !sea.ptr<64>
  %eight = sea.const 8 : i64
  %a1 = sea.memmove %a[%p], %b[%p], %eight inplace : !m
  %x = sea.load %b[%p] : !m -> i8
  sea.halt
}

// A placed object's sea.segment proves alignment by its numeric base.
// CHECK-LABEL: sea.program @placed_segment
// CHECK: sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i64, aligned>
sea.program @placed_segment {
  %b = sea.segment "stack.1" base 3221225456 : i64 size 16 : i64 align 8 kind "stack" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
  %eight = sea.const 8 : i64
  %q = sea.addr.add %b, %eight : !sea.addr<64>, i64
  %x = sea.load %m0[%q] : !sea.mem<!sea.addr<64>, i8> -> i64
  sea.halt
}

// A base that is not a multiple of k does not prove it (the memory stays
// bytes, whatever the object's declared align).
// CHECK-LABEL: sea.program @placed_segment_odd
// CHECK: sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
sea.program @placed_segment_odd {
  %b = sea.segment "stack.1" base 3221225460 : i64 size 16 : i64 align 8 kind "stack" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [0] : !sea.mem<!sea.addr<64>, i8>
  %x = sea.load %m0[%b] : !sea.mem<!sea.addr<64>, i8> -> i64
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
