// --fuzz-serve: one JSON request per stdin line (the REQ lines below), one
// JSON response per line. Coverage novelty is per server: a request that
// reaches nothing new gets empty lists. A bad request gets an error record
// and the server goes on. The input of a response replays with
// --input-file.
// RUN: grep '^// REQ: ' %s | sed 's|^// REQ: ||' | sea-run %s --program=main --fuzz-serve > %t.out
// RUN: FileCheck %s < %t.out
// RUN: %python -c "import json, sys; r = [json.loads(l) for l in open(sys.argv[1])][5]; open(sys.argv[2], 'w').write('\n'.join(r['input']) + '\n')" %t.out %t.replay
// RUN: sea-run %s --program=main --input-file=%t.replay | FileCheck %s --check-prefix=REPLAY
// A memory-safety finding with --defer-memory-errors: the fields of a plain run.
// RUN: echo '{"inputs": {"2": 9}}' | sea-run %s --program=mem --fuzz-serve --defer-memory-errors | FileCheck %s --check-prefix=MEM
// The server's --missing is the default of the requests.
// RUN: echo '{"inputs": {"1": 5}}' | sea-run %s --program=main --fuzz-serve --missing=zero | FileCheck %s --check-prefix=ZERO
// RUN: not sea-run %s --program=main --fuzz-serve --fuzz=10 < /dev/null 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not sea-run %s --program=main --fuzz-serve --format=text < /dev/null 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not sea-run --fuzz-serve < %s 2>&1 | FileCheck %s --check-prefix=STDIN

// REQ: {"inputs": {"0": 1, "1": 2}, "id": "a"}
// REQ: {"inputs": {"0": 1, "1": "2"}}
// REQ: {"inputs": {"0": 3, "1": 2}}
// REQ: not json
// REQ: {"inputs": {"0": "0x100", "1": 2}}
// REQ: {"inputs": {"0": "3", "1": "0x28"}}
// REQ: {"inputs": {"1": 5}, "missing": "random", "seed": 7, "trace_sites": true}
// REQ: {"inputs": {"1": 5}}
// REQ: {"inputs": {"7": 1}}
// REQ: {"inputs": {"0": 1}, "trace": true}

// CHECK: {"kind":"response","request":1,"id":"a","status":"halted",{{.*}}"new":{"blocks":[0,1],"edges":{{\[\[}}-1,0],[0,1]],"assumes":[0,1]},"coverage":{"blocks":2,"blocks_total":3,"edges":2,"assumes":2,"assumes_total":2},"input":["0=1","1=2"],"steps":
// CHECK-NEXT: {"kind":"response","request":2,"status":"halted",{{.*}}"new":{"blocks":[],"edges":[],"assumes":[]},
// CHECK-NEXT: {"kind":"response","request":3,"status":"assume-blocked",{{.*}}"blocked":1,"blocked_at":"{{.*}}serve.mlir:[[@LINE+29]]:3",
// CHECK-NEXT: {"kind":"error","request":4,"error":"not JSON:
// CHECK-NEXT: {"kind":"error","request":5,"error":"'0=0x100': not an i8 value"
// CHECK-NEXT: {"kind":"response","request":6,"status":"assert-failed",{{.*}}"at":"{{.*}}serve.mlir:[[@LINE+29]]:{{.*}}"path":[{{.*}}"new":{"blocks":[2],"edges":{{\[\[}}0,2]],"assumes":[]},{{.*}}"input":["0=3","1=40"],
// CHECK-NEXT: {"kind":"response","request":7,{{.*}}"reads":[{"site":0,"width":8,"value":{{[0-9]+}},"origin":"random"}{{.*}}
// CHECK-NEXT: {"kind":"response","request":8,"status":"error",{{.*}}"message":"no input for nondet site 0"
// CHECK-NEXT: {"kind":"error","request":9,"error":"'7=1': the program has no input site 7"
// CHECK-NEXT: {"kind":"error","request":10,"error":"unknown request field \"trace\"
// CHECK-NEXT: {"kind":"summary","requests":10,"errors":4,"coverage":{"blocks":3,"blocks_total":3,"edges":3,"assumes":2,"assumes_total":2}
// CHECK-NOT: {

// REPLAY: nondet: site=0 {{.*}} value=3
// REPLAY: nondet: site=1 {{.*}} value=40
// REPLAY: status: assert-failed

// MEM: {"kind":"response","request":1,"status":"memory-error",{{.*}}"reached":"halted",{{.*}}"deferred":1,"deferred_kind":"out-of-bounds",{{.*}}"input":["2=9"],

// ZERO: "status":"halted",{{.*}}"input":["0=0","1=5"]

// BAD: --fuzz-serve does not take
// STDIN: --fuzz-serve reads requests on stdin; give the program as a file

sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %y = sea.nondet "nondet_u8" [1] : i8
  %c100 = sea.const 100 : i8
  %lt = sea.icmp ult, %y, %c100 : i8
  sea.assume %lt
  %le = sea.icmp ule, %x, %y : i8
  sea.assume %le
  %d = sea.sub %y, %x : i8
  %c37 = sea.const 37 : i8
  %ne = sea.icmp ne, %d, %c37 : i8
  sea.cond_br %ne, ^ok, ^bad
^ok:
  sea.halt
^bad:
  sea.assert %ne
  sea.halt
}

!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @mem {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 4 : i64
  %p = sea.alloca %n align 1 [1] : i64 -> !sea.ptr<64>
  %i = sea.nondet "nondet_u64" [2] : i64
  %q = sea.ptr.add %p, %i : !sea.ptr<64>, i64
  %v = sea.load %m0[%q] : !pm -> i8
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
