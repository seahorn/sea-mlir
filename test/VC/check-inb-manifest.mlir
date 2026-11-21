// sea-vc --check-inb writes every part and manifest.json into the job
// directory before any part runs (docs/design/CheckInb.md, "The
// manifest"); an outside runner can run the parts from the manifest alone.
//
// --vcgen-only: the parts and the manifest, no part run, the directory
// kept and printed, result: none.
// RUN: rm -rf %t.d
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off --vcgen-only --check-inb-dir=%t.d | FileCheck %s --check-prefix=PREP
// RUN: test -f %t.d/manifest.json
// RUN: test -f %t.d/part-0.mlir
// RUN: test -f %t.d/part-1.mlir
// RUN: test ! -e %t.d/part-0.jsonl
// RUN: test ! -e %t.d/part-1.jsonl
// RUN: FileCheck %s --check-prefix=MANIFEST < %t.d/manifest.json
// A part run by hand from its manifest argv gives the job's result.
// RUN: %python %S/Inputs/run-manifest-part.py %t.d 0 | FileCheck %s --check-prefix=HAND0
// RUN: %python %S/Inputs/run-manifest-part.py %t.d 1 | FileCheck %s --check-prefix=HAND1
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=JOB
// --check-inb-keep: the temporary directory stays, with its manifest.
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off --check-inb-keep > %t.keep.txt
// RUN: %python %S/Inputs/check-dir.py %t.keep.txt | FileCheck %s --check-prefix=KEEP
// Without it, no directory line.
// RUN: sea-vc %s --check-inb --lower --seam --ackermannize=off | FileCheck %s --check-prefix=NOKEEP

// PREP: check-inb: checks=2 static=0 left=2
// PREP: check-inb-dir: {{.*}}.d
// PREP: check-inb.manifest: {{.*}}manifest.json parts=2
// PREP-NOT: check-inb.part:
// PREP: result: none

// MANIFEST: "format": "sea-vc-check-inb"
// MANIFEST: "parts": [
// MANIFEST: "argv": [
// MANIFEST-NEXT: "{{.*}}sea-vc"
// MANIFEST-NEXT: "{{.*}}part-0.mlir"
// MANIFEST-NOT: vcgen-only
// MANIFEST: "--check-inb=false"
// MANIFEST: "check_id": 0
// MANIFEST: "index": 0
// MANIFEST: "argv": [
// MANIFEST-NEXT: "{{.*}}sea-vc"
// MANIFEST-NEXT: "{{.*}}part-1.mlir"
// MANIFEST: "index": 1
// MANIFEST: "version": 1

// HAND0: part 0 result=sat status=0
// HAND1: part 1 result=unsat status=0
// JOB: check-inb.part: 0 {{.*}}result=sat status=0
// JOB: check-inb.part: 1 {{.*}}result=unsat status=0

// KEEP: dir-exists
// KEEP: manifest-exists
// NOKEEP-NOT: check-inb-dir:
// NOKEEP: result: sat

!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ag {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [2] : i64
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%p], %v inplace : !m, i8
  %c15 = sea.const 15 : i64
  %k = sea.sub %c15, %n : i64
  %q = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %m2 = sea.store %m1[%q], %v inplace : !m, i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
