// Logs in sea-vc (docs/reference/logging.md): the tag registry, the warnings
// of the selection, the log file rules, MLIR's pass options. The selection
// and the event formats: log-select.test.

// --log=help: the registered tags, sorted, with their max level.
// RUN: sea-vc --log=help | FileCheck %s --check-prefix=HELP
// HELP: log tags
// HELP: lower.pass 1
// HELP: vc.phase{{ +}}1
// HELP: vc.solve 1
// HELP: vc.store-read 3
// HELP: warn.<kind> 2

// Off unless selected.
// RUN: sea-vc %s 2>&1 | FileCheck %s --check-prefix=OFF --implicit-check-not=log:
// OFF: result: sat

// An entry that selects no tag is a warning, not an error; so is a
// malformed one.
// RUN: sea-vc %s --log=no.such-tag 2>&1 | FileCheck %s --check-prefix=UNKNOWN
// RUN: env SEA_LOG=no.such-tag sea-vc %s 2>&1 | FileCheck %s --check-prefix=UNKNOWN-ENV
// UNKNOWN: Warning: --log: no log tag matches 'no.such-tag'
// UNKNOWN-SAME: [log.unknown-tag]
// UNKNOWN: result: sat
// UNKNOWN-ENV: Warning: SEA_LOG: no log tag matches 'no.such-tag'
// UNKNOWN-ENV: result: sat
// RUN: sea-vc %s --log=vc.store-read:7 2>&1 | FileCheck %s --check-prefix=BAD
// BAD: Warning: --log: 'vc.store-read:7': the level is 0-3 (ignored) [log.bad-spec]
// BAD: result: sat

// The log file is opened at the first event: no event, no file. A
// SEA_LOG_FILE that cannot be opened does not stop the tool; an explicit
// --log-file that cannot be written is an error.
// RUN: rm -rf %t.unused.log %t.nodir
// RUN: sea-vc %s --log-file=%t.unused.log
// RUN: env SEA_LOG_FILE=%t.unused.log sea-vc %s
// RUN: not ls %t.unused.log
// RUN: env SEA_LOG_FILE=%t.nodir/x.log sea-vc %s 2>&1 | FileCheck %s --check-prefix=OFF
// RUN: not sea-vc %s --log-file=%t.nodir/x.log 2>&1 | FileCheck %s --check-prefix=FLAGBAD
// FLAGBAD: Error: --log-file: cannot write '{{.*}}x.log' [log.file]
// FLAGBAD-NOT: result:

// MLIR's pass-manager and timing options on sea-vc's pass managers.
// RUN: sea-vc %s --lower --mlir-print-ir-after-all 2>&1 | FileCheck %s --check-prefix=IR
// IR: IR Dump After
// --mlir-print-ir-module-scope needs --mlir-disable-threading.
// RUN: sea-vc %s --lower --mlir-print-ir-after-all --mlir-print-ir-module-scope --mlir-disable-threading 2>&1 | FileCheck %s --check-prefix=SCOPE
// SCOPE: IR Dump After
// SCOPE-NEXT: module {
// SCOPE: result: sat
// RUN: sea-vc %s --lower --mlir-timing 2>&1 | FileCheck %s --check-prefix=TIMING
// TIMING: Execution time report

sea.program @main {
  %x = sea.nondet "nondet_u8" [0] : i8
  %c3 = sea.const 3 : i8
  %ne = sea.icmp ne, %x, %c3 : i8
  sea.assert %ne
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
