// Every tool prints the release version and `git describe` (--version), and
// sea-vc's output carries it first, in text and in jsonl (docs/Release.md).
// RUN: sea-vc --version | FileCheck %s --check-prefix=VC
// RUN: sea-run --version | FileCheck %s --check-prefix=TOOL -DNAME=run
// RUN: sea-opt --version | FileCheck %s --check-prefix=TOOL -DNAME=opt
// RUN: sea-reduce --version | FileCheck %s --check-prefix=TOOL -DNAME=reduce
// RUN: sea-expr-eval --version | FileCheck %s --check-prefix=TOOL -DNAME=expr-eval
// RUN: printf 'bin bvadd 8 200 100\n' | sea-expr-eval --solver=z3 2>%t.err | FileCheck %s --check-prefix=EVAL
// RUN: FileCheck %s --check-prefix=EVALERR --input-file=%t.err
// RUN: sea-vc %s | FileCheck %s --check-prefix=TEXT
// RUN: sea-vc %s --format=jsonl | FileCheck %s --check-prefix=JSONL
// RUN: sea-vc %s --smt2 2>&1 >/dev/null | FileCheck %s --check-prefix=SMT
// VC: sea-vc (sea-mlir {{[0-9]+\.[0-9]+\.[0-9]+}} ({{.+}}))
// VC-NEXT: LLVM/MLIR {{.*}} at {{[0-9a-f]+}}
// VC-NEXT: build:
// VC-NEXT: z3 {{[0-9.]+}}
// VC-NEXT: yices{{( [0-9.]+|: not built .*)}}
// VC-NEXT: bitwuzla
// TOOL: sea-[[NAME]] (sea-mlir {{[0-9]+\.[0-9]+\.[0-9]+}} ({{.+}}))
// TOOL-NEXT: LLVM/MLIR
// EVAL: {{^}}44{{$}}
// EVAL-NOT: tool.version
// EVALERR: {{^}}tool.version: sea-mlir {{[0-9]+\.[0-9]+\.[0-9]+}} ({{.+}}){{$}}
// TEXT: {{^}}tool.version: sea-mlir {{[0-9]+\.[0-9]+\.[0-9]+}} ({{.+}}){{$}}
// TEXT: program: @p
// SMT: {{^}}tool.version: sea-mlir
// JSONL: {"kind":"stat","key":"tool.version","value":"sea-mlir {{[0-9.]+}} ({{.+}})","text":"tool.version: sea-mlir
// JSONL: {"kind":"program"
sea.program @p {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c100 = sea.const 100 : i8
  %y = sea.add %x, %c100 : i8
  %ok = sea.icmp uge, %y, %c100 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
