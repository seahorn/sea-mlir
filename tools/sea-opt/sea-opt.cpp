// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- sea-opt.cpp - sea optimizer driver -----------------------*- C++ -*-===//

#include "mlir/IR/DialectRegistry.h"
#include "mlir/Tools/mlir-opt/MlirOptMain.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include "sea/Dialect/Sea/SeaDialect.h"
#include "sea/Reduce/Reduce.h"
#include "sea/Support/Diag.h"
#include "sea/Support/Version.h"
#include "sea/Transforms/Passes.h"

static const char *const agentGuide = R"(sea-opt agent guide (plain, terse)

WHAT: the mlir-opt driver for the sea dialect: parse, verify, print, and
run passes on sea programs.

WHY BEAT MANUAL: the verifier checks what eyeballing misses (types of
block arguments, unique nondet sites, cast widths, terminators). Printing
through sea-opt gives the canonical form to diff against.

TYPICAL:
  sea-opt p.mlir                          # parse + verify + print
  sea-opt p.mlir --mlir-print-op-generic  # generic form (stable, simple)
  sea-opt p.mlir -split-input-file -verify-diagnostics  # negative tests
  sea-opt p.mlir --canonicalize --cse     # upstream passes work on sea
  sea-opt p.mlir --sccp                   # fold constants: pure ops on
                                          #   constants -> sea.const (values
                                          #   by evalPureOp); nsw/nuw ops,
                                          #   nondets, memory, pointers stay
  sea-opt p.mlir --sea-place-static       # abstract pointers -> addresses
  sea-opt p.mlir --sea-expand-intrinsics  # umin, bswap, fshl, uaddo.. -> core ops
  sea-opt p.mlir --sea-scalarize-structs  # struct values -> their leaves
  sea-opt p.mlir --sea-expand-memset      # symbolic-length memset -> bounded
                                          #   store chain + assume(n <= 16)
  sea-opt p.mlir --pass-pipeline='builtin.module(sea.program(sea-wrap-flags{mode=assume}))'
                                          # nsw/nuw promises -> assume|assert|drop
  sea-opt p.mlir --sea-widen-memory="k=8 report=true"
                                          # byte memories whose accesses are
                                          #   all aligned whole k-byte words
                                          #   -> !sea.mem<I, i64, aligned>;
                                          #   report: why each memory widened
                                          #   or not (run sccp first)
  sea-opt p.mlir --sea-apply-tracking     # sea.tracking_on/off markers (at
                                          #   dominating positions) -> `tracked`
                                          #   on the updates between them
  sea-opt p.mlir --sea-lower-tracking     # is_modified / reset_modified /
                                          #   tracked -> a shadow memory chain
                                          #   keyed by sea.ptr.base
  sea-opt p.mlir --sea-lower-fat-pointers # pointer offset / object size as
                                          #   explicit companions (also in
                                          #   pointer-holding memory); then
                                          #   ptr.base / dereferenceable place
  sea-opt p.mlir --sea-print-strengthen --mlir-print-debuginfo
                                          # weak prints (names) -> strong
                                          #   (SSA operands, kept alive);
                                          #   --sea-print-weaken: back.
                                          #   Print names live in locations:
                                          #   keep --mlir-print-debuginfo
                                          #   (docs/design/Print.md)
  sea-opt p.mlir --sea-vc-lower           # what sea-vc --lower runs (placed input)
  sea-opt p.mlir --pass-pipeline='builtin.module(sea-vc-lower{fold=false})'
                                          # the same without sccp (keeps every
                                          #   op's location)
  sea-opt p.mlir --pass-pipeline='builtin.module(sea.program(sea-place-static{symbolic-size=assert}))'
  sea-opt --help                          # all passes and flags

OUTPUT: the module on stdout; diagnostics on stderr as
`<file>:<line>:<col>: error: <message>`.

STATUS CODES: 0 ok; 1 parse, verify or pass failure.
)";

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i)
    if (llvm::StringRef(argv[i]) == "--agent" ||
        llvm::StringRef(argv[i]) == "-agent") {
      llvm::outs() << agentGuide;
      return 0;
    }
  sea::setVersionPrinter("sea-opt");
  mlir::registerTransformsPasses();
  sea::registerPasses();
  sea::registerReducePasses();
  struct Summary {
    ~Summary() { sea::diag::printSummary(); }
  } summary;

  mlir::DialectRegistry registry;
  registry.insert<sea::SeaDialect>();

  return mlir::asMainReturnCode(
      mlir::MlirOptMain(argc, argv, "sea optimizer driver\n", registry));
}
