// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Diag.h - visible, non-flooding warnings ------------------*- C++ -*-===//
//
// Every unimplemented or incomplete feature must be visible. A warning kind
// (a short stable tag such as "vc.mem-op-expanded") prints in color the first
// time it occurs, with the location; later occurrences are counted, and
// printed only when the kind is selected with --log=warn.<kind>:2 (or the
// old spelling --log=<kind>; sea/Support/Log.h). At exit, printSummary reports
// the kinds that occurred more than once, so one issue cannot drown the log and
// none goes unnoticed.
//
// Errors (unsupported input the tool cannot handle) print in red; the caller
// stops. Colors appear only on a terminal. Thread-safe (MLIR runs passes on
// different programs in parallel).
//
//===----------------------------------------------------------------------===//

#ifndef SEA_SUPPORT_DIAG_H
#define SEA_SUPPORT_DIAG_H

#include "mlir/IR/Location.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>

namespace sea::diag {

/// Reports a warning of `kind`. First occurrence: printed (yellow). Later:
/// counted; printed only if `kind` is selected with --log=warn.<kind>:2.
void warn(llvm::StringRef kind, const llvm::Twine &msg,
          std::optional<mlir::Location> loc = std::nullopt);

/// Reports an error of `kind` (red). Always printed; the caller stops.
void error(llvm::StringRef kind, const llvm::Twine &msg,
           std::optional<mlir::Location> loc = std::nullopt);

/// How many times `kind` was reported (warnings and errors).
unsigned count(llvm::StringRef kind);

/// Prints one line per warning kind that occurred more than once. Call at
/// tool exit.
void printSummary(llvm::raw_ostream &os = llvm::errs());

} // namespace sea::diag

#endif // SEA_SUPPORT_DIAG_H
