// Option guards of --fuzz.
// RUN: not sea-run %s --corpus=%s 2>&1 | FileCheck %s --check-prefix=NEEDS
// RUN: not sea-run %s --fuzz-time=-1 2>&1 | FileCheck %s --check-prefix=NEG
// RUN: rm -rf %t && mkdir -p %t/dir && echo '0=1' > %t/dir/a.txt
// RUN: not sea-run %s --input-file=%t/dir 2>&1 | FileCheck %s --check-prefix=DIR
// RUN: not sea-run %s --fuzz=10 --fuzz-out=%t/dir 2>&1 | FileCheck %s --check-prefix=NONEMPTY
// RUN: mkdir -p %t/empty && sea-run %s --fuzz=10 --fuzz-out=%t/empty | FileCheck %s --check-prefix=EMPTY

// NEEDS: --corpus, {{.*}} need --fuzz or --fuzz-time
// NEG: --fuzz-time must not be negative
// DIR: --input-file '{{.*}}dir' is a directory; give one corpus file
// NONEMPTY: --fuzz-out directory '{{.*}}dir' is not empty
// EMPTY: fuzz.trials: 10
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
