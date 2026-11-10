// --fuzz-sites: one fuzz.site line per input site, in program order, then
// exit (no run). sea.ptr.nondet takes no input and is not listed.
// RUN: sea-run %s --program=main --fuzz-sites | FileCheck %s
// RUN: sea-run %s --program=main --fuzz-sites --format=jsonl > %t.jsonl
// RUN: FileCheck %s --check-prefix=JSONL < %t.jsonl
// RUN: sea-run %s --program=main --fuzz-sites > %t.txt
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl --require stat:callee,width,input,op,at
// RUN: not sea-run %s --program=main --fuzz-sites --fuzz=10 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not sea-run %s --program=main --fuzz-sites --seed=1 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not sea-run %s --program=main --fuzz-sites --max-steps=5 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not sea-run %s --program=main --fuzz-sites --defer-memory-errors 2>&1 | FileCheck %s --check-prefix=BAD
// A program without input sites: the count says so.
// RUN: sea-run %s --program=none --fuzz-sites | FileCheck %s --check-prefix=NONE
// NONE: fuzz.sites: 0 value=0 memory=0

// CHECK: fuzz.site: 8 callee=nd_size_t width=64 input=value op=sea.nondet at={{.*}}sites.mlir:[[@LINE+10]]:
// CHECK-NEXT: fuzz.site: 3 callee=nd_pair width=40 input=value op=sea.struct.nondet at={{.*}}sites.mlir:[[@LINE+10]]:
// CHECK-NEXT: fuzz.site: 4 callee=heap width=8 input=memory op=sea.mem.nondet at={{.*}}sites.mlir:[[@LINE+10]]:
// CHECK-NEXT: fuzz.site: 6 callee=- width=8 input=memory op=sea.memhavoc at={{.*}}sites.mlir:[[@LINE+14]]:
// CHECK-NEXT: fuzz.sites: 4 value=2 memory=2
// CHECK-NOT: fuzz.site
// CHECK-NOT: status:
// JSONL: {"kind":"stat","key":"fuzz.site","value":8,"callee":"nd_size_t","width":64,"input":"value","op":"sea.nondet","at":"{{.*}}","text":"fuzz.site: 8 {{.*}}"}
// BAD: --fuzz-sites lists the input sites and exits
sea.program @main {
  %x = sea.nondet "nd_size_t" [8] : i64
  %s = sea.struct.nondet "nd_pair" [3] : !sea.struct<i8, i32>
  %m = sea.mem.nondet "heap" [4] : !sea.mem<!sea.addr<64>, i8>
  %p = sea.ptr.nondet "nd_node" [5] : !sea.ptr<64>
  %c16 = sea.const 16 : i64
  %a = sea.addr.from_int %c16 : i64 to !sea.addr<64>
  %c4 = sea.const 4 : i64
  %m2 = sea.memhavoc %m[%a], %c4 [6] inplace : !sea.mem<!sea.addr<64>, i8>
  sea.halt
}

sea.program @none {
  %p = sea.ptr.nondet "nd_node" [0] : !sea.ptr<64>
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
