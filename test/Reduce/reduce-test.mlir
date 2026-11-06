// utils/reduce-test.py, the interestingness tests: each mode on a case
// that has the property (exit 1), and controls that must not count (exit
// 0): a parse error, a sea-vc error, the wrong verdict.
//
// This program is what `reduce-test.py reduce --mode verdict-differs`
// made of a larger one: an out-of-bounds store into @a that reaches @b
// once placed. sea-vc --lower (no SeaM) says sat; --seam, which assumes
// A-INB (every access stays in its object), says unsat. Not a bug: the
// disagreement is the assumption, which a bounds-check job discharges.
//
// RUN: not %python %sea_src_root/utils/reduce-test.py test --mode verdict-differs --a=--lower --b="--lower --seam" --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode verdict-differs --a=--lower --b="--lower --seam" --a-result=unsat --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode verdict-differs --a=--lower --b=--lower --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode verdict-differs --a=--lower --b="--lower --seam" --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %S/Inputs/parse-error.mlir
//
// result: the verdict and status asked for; a parse error is not "unsat".
// RUN: not %python %sea_src_root/utils/reduce-test.py test --mode result --config="--lower --seam" --result unsat --status 0 --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode result --config="--lower" --result unsat --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode result --config="--lower" --result sat --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %S/Inputs/parse-error.mlir
//
// crash: a diagnostic of sea-vc (without --lower an abstract program is
// refused); the parse error prints an error too, but does not count.
// RUN: not %python %sea_src_root/utils/reduce-test.py test --mode crash --config= --match "vc.abstract-program" --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode crash --config= --match "vc.abstract-program" --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %S/Inputs/parse-error.mlir
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode crash --config=--lower --signal any --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
//
// slow, on a --seam-stats counter: A (simplification on) fires more rules
// than B (off); budgets on both sides.
// RUN: not %python %sea_src_root/utils/reduce-test.py test --mode slow --metric seam.rules-fired --a="--lower --seam --seam-stats" --b="--lower --seam --seam-stats --seam-simplify=0" --a-over 3 --b-under 3 --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode slow --metric seam.rules-fired --a="--lower --seam --seam-stats" --b="--lower --seam --seam-stats --seam-simplify=0" --a-over 3 --b-under 2 --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
// RUN: %python %sea_src_root/utils/reduce-test.py test --mode slow --metric seam.rules-fired --a="--lower --seam --seam-stats" --b="--lower --seam --seam-stats --seam-simplify=0" --a-over 3 --b-under 3 --min-ratio 4 --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt %s
//
// script: the generated test script behaves as the test.
// RUN: %python %sea_src_root/utils/reduce-test.py script --mode verdict-differs --a=--lower --b="--lower --seam" --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt -o %t.sh
// RUN: not %t.sh %s
// RUN: %t.sh %S/Inputs/parse-error.mlir

sea.program @main {
  %m0 = sea.mem.nondet "heap" [0] : !sea.mem<!sea.ptr<64>, i8>
  %a = sea.global @a size 4 align 4 : !sea.ptr<64>
  %b = sea.global @b size 4 align 4 : !sea.ptr<64>
  %z = sea.const 0 : i8
  %one = sea.const 1 : i8
  %m1 = sea.store %m0[%b], %z inplace : !sea.mem<!sea.ptr<64>, i8>, i8
  %k = sea.nondet "nd" [2] : i64
  %pk = sea.ptr.add %a, %k : !sea.ptr<64>, i64
  %m2 = sea.store %m1[%pk], %one inplace : !sea.mem<!sea.ptr<64>, i8>, i8
  %x = sea.load %m2[%b] : !sea.mem<!sea.ptr<64>, i8> -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
