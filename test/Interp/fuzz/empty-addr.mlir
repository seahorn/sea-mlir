// `<site>@` with no address is an error in every input syntax (it used to
// be read as the value of site 0): --input, a corpus file, a serve request.
// RUN: not sea-run %s --input=0@=1 2>&1 | FileCheck %s
// RUN: printf '0@=1\n' > %t.txt
// RUN: not sea-run %s --input-file=%t.txt 2>&1 | FileCheck %s
// RUN: echo '{"inputs": {"0@": 1}}' | sea-run %s --fuzz-serve | FileCheck %s --check-prefix=SERVE
// CHECK: expected <site>@<addr>=<value>
// SERVE: {"kind":"error","request":1,"error":"bad input \"0@\": expected <site>@<addr>=<value>
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
