// --fuzz-dict-share=P: the probability that a fresh value and that a
// mutation is a dictionary value. The guard assume(x == 1234) passes only
// with a dictionary value (1 in 3: 1233, 1234, 1235): P=0 never passes it,
// P=1 passes it within a few trials. Unset is the original mix (dict.mlir).
// RUN: sea-run %s --fuzz=2000 --seed=1 --fuzz-dict-share=0 | FileCheck %s --check-prefix=ZERO
// RUN: sea-run %s --fuzz=50 --seed=1 --fuzz-dict-share=1 | FileCheck %s --check-prefix=ONE
// RUN: not sea-run %s --fuzz=5 --fuzz-dict-share=1.5 2>&1 | FileCheck %s --check-prefix=RANGE
// RUN: not sea-run %s --fuzz=5 --fuzz-dict-share=0.5 --fuzz-dict=false 2>&1 | FileCheck %s --check-prefix=BOTH
// RUN: not sea-run %s --fuzz-dict-share=0.5 2>&1 | FileCheck %s --check-prefix=NEEDS

// ZERO-NOT: status:
// ZERO: fuzz.status: halted=0 assert-failed=0 assume-blocked=2000
// ZERO: fuzz.corpus: {{.*}} dictionary=3
// ZERO-NOT: fuzz.failure
// ONE: status: assert-failed
// ONE: fuzz.failure: 1 trial=
// RANGE: --fuzz-dict-share must be in [0, 1]
// BOTH: --fuzz-dict-share does not go with --fuzz-dict=false
// NEEDS: need --fuzz or --fuzz-time
sea.program @main {
  %x = sea.nondet "nondet_u32" [0] : i32
  %c = sea.const 1234 : i32
  %eq = sea.icmp eq, %x, %c : i32
  sea.assume %eq
  %f = sea.const 0 : i1
  sea.assert %f
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
