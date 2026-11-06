// A candidate sea-vc refuses (BMC form: two asserts, a critical edge) is
// uninteresting in every mode, even where the verdict would match: the
// test runs sea-vc without --lower (BMC form required) and logs the
// outcome `sea-vc-error` with the diagnostic kind. Only crash mode may
// target a diagnostic, and only the one it matches.
//
// RT: the wrapper with the tools of this build.
// DEFINE: %{rt} = %python %sea_src_root/utils/reduce-test.py test --sea-vc=%sea_vc --sea-opt=%sea_bin/sea-opt --log=%t.log
// RUN: rm -f %t.log
// The valid program (this file) is interesting in result mode (sat) ...
// RUN: not %{rt} --mode result --config= --result sat %s
// ... the two broken ones are not, in any mode:
// RUN: %{rt} --mode result --config= --result sat %S/Inputs/two-asserts.mlir
// RUN: %{rt} --mode result --config= --result sat %S/Inputs/critical-edge.mlir
// RUN: %{rt} --mode verdict-differs --a= --b=--seam %S/Inputs/two-asserts.mlir
// RUN: %{rt} --mode slow --metric time --a=--timeout=0 --b=--timeout=0 --a-over 0 --b-under 0 %S/Inputs/critical-edge.mlir
// RUN: %{rt} --mode crash --config= --match "result: sat|error" --signal any %S/Inputs/two-asserts.mlir
// RUN: %{rt} --mode crash --config= --match "result" %S/Inputs/critical-edge.mlir
// The diagnostic itself, when it is what crash mode asks for, counts:
// RUN: not %{rt} --mode crash --config= --match "BMC form" %S/Inputs/critical-edge.mlir
// RUN: FileCheck %s --input-file=%t.log
// CHECK: outcome=interesting
// CHECK: outcome=sea-vc-error kind=BMC_form interesting=0
// CHECK: outcome=sea-vc-error kind=BMC_form interesting=0
// CHECK: outcome=sea-vc-error kind=BMC_form interesting=0
// CHECK: outcome=sea-vc-error kind=BMC_form interesting=0
// CHECK: outcome=sea-vc-error kind=BMC_form interesting=0
// CHECK: outcome=sea-vc-error kind=BMC_form interesting=0
// CHECK: outcome=interesting
// RUN: %python %sea_src_root/utils/reduce-test.py summary %t.log | FileCheck %s --check-prefix=SUM
// SUM: summary.stage: total generated=8 cached=0 verifier-rejected=0 verify-failed=0 sea-vc-error=6 crashed=0 timeout=0 uninteresting=0 interesting=2
// SUM: summary.kind: stage=- outcome=sea-vc-error kind=BMC_form n=6

sea.program @main {
  %a = sea.nondet "nd" [0] : i8
  %c1 = sea.const 1 : i8
  %z = sea.icmp ne, %a, %c1 : i8
  sea.assert %z
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
