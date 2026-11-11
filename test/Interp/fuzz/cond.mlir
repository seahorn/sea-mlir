// The cond= field of fuzz.blocked: each blocking assume's condition over
// input sites. Each seed blocks at one assume (five runs, one per assume).
// RUN: rm -rf %t && mkdir -p %t
// RUN: printf '0=5\n1=3\n' > %t/a.txt
// RUN: printf '0=1\n1=20\n' > %t/b.txt
// RUN: printf '0=1\n1=7\n' > %t/c.txt
// RUN: printf '0=2\n1=3\n' > %t/d.txt
// RUN: printf '0=1\n1=3\n2@16=5\n' > %t/e.txt
// RUN: sea-run %s --fuzz=5 --corpus=%t | FileCheck %s
// RUN: sea-run %s --fuzz=5 --corpus=%t --format=jsonl > %t.jsonl
// RUN: FileCheck %s --check-prefix=JSONL < %t.jsonl
// RUN: sea-run %s --fuzz=5 --corpus=%t > %t.txt
// RUN: %python %sea_src_root/utils/jsonl-check.py --text %t.txt --jsonl %t.jsonl --require stat:sites,cond

// relational: two sites
// CHECK: fuzz.blocked: 1 op=sea.assume at={{.*}}cond.mlir:[[@LINE+21]]:3 sites=0,1 cond=(ule s0 s1)
// a constant comparison
// CHECK-NEXT: fuzz.blocked: 1 op=sea.assume at={{.*}}cond.mlir:[[@LINE+21]]:3 sites=1 cond=(ult s1 10)
// through a block argument with one incoming value
// CHECK-NEXT: fuzz.blocked: 1 op=sea.assume at={{.*}}cond.mlir:[[@LINE+23]]:3 sites=1 cond=(ne s1 7)
// cut off: a block argument with two incoming values
// CHECK-NEXT: fuzz.blocked: 1 op=sea.assume at={{.*}}cond.mlir:[[@LINE+29]]:3 sites=- cond=?
// a load straight from a sea.mem.nondet (its cells are site 2's inputs)
// CHECK-NEXT: fuzz.blocked: 1 op=sea.assume at={{.*}}cond.mlir:[[@LINE+33]]:3 sites=2 cond=(eq (load:8 s2@(addr.from_int 16)) 0)
// CHECK-NOT: fuzz.blocked
// JSONL: {"kind":"stat","key":"fuzz.blocked","value":1,"op":"sea.assume","at":"{{.*}}","sites":"0,1","cond":"(ule s0 s1)","text":"fuzz.blocked: 1 {{.*}} sites=0,1 cond=(ule s0 s1)"}
sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %m = sea.mem.nondet "heap" [2] : !sea.mem<!sea.addr<64>, i8>
  %c1 = sea.const 1 : i8
  %c7 = sea.const 7 : i8
  %c10 = sea.const 10 : i8
  %t = sea.const 1 : i1
  %f = sea.const 0 : i1
  %le = sea.icmp ule, %x, %y : i8
  sea.assume %le
  %lt = sea.icmp ult, %y, %c10 : i8
  sea.assume %lt
  sea.br ^one(%y : i8)
^one(%z: i8):
  %ne = sea.icmp ne, %z, %c7 : i8
  sea.assume %ne
  %isone = sea.icmp eq, %x, %c1 : i8
  sea.cond_br %isone, ^a, ^b
^a:
  sea.br ^m(%t : i1)
^b:
  sea.br ^m(%f : i1)
^m(%p: i1):
  sea.assume %p
  %c16 = sea.const 16 : i64
  %a16 = sea.addr.from_int %c16 : i64 to !sea.addr<64>
  %v = sea.load %m[%a16] : !sea.mem<!sea.addr<64>, i8> -> i8
  %z0 = sea.const 0 : i8
  %iz = sea.icmp eq, %v, %z0 : i8
  sea.assume %iz
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
