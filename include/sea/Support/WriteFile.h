// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- WriteFile.h - output files written whole -----------------*- C++ -*-===//
//
// writeFileAtomic: an output file (a placement, a witness, a corpus entry,
// a dump) is written to `.<name>.tmp` in its directory, then renamed over
// the target, so a reader sees it complete or not at all, and a failed
// write (no such directory, a full disk: also errors that show only at
// close) is reported, never a silently truncated file. On failure the
// temporary is removed.
//
// The path "-" is stdout, and an existing target that is not a regular
// file (/dev/stdout, /dev/stderr, a FIFO) is written directly, without a
// temporary and a rename; the errors at open and at close are reported the
// same way.
//
// Limits: the temporary name is fixed (`.<name>.tmp`), so two writers of
// the same target race; the rename replaces a symlink target (it does not
// write through it) and the new file does not keep the mode of an existing
// one.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_SUPPORT_WRITEFILE_H
#define SEA_SUPPORT_WRITEFILE_H

#include "mlir/IR/Location.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/LogicalResult.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>

namespace sea {

/// The contents of a file: writes them to the stream; a failure (reported
/// by the body) abandons the file.
using WriteBody = llvm::function_ref<llvm::LogicalResult(llvm::raw_ostream &)>;

/// Writes \p path whole (see the file comment). On failure: \p error =
/// "cannot write '<path>': <reason>" (or the rename's reason), unless the
/// body failed (then empty: the body reported it).
llvm::LogicalResult writeFileAtomic(llvm::StringRef path, WriteBody body,
                                    std::string &error);

/// The same; a write failure is diag::error(\p kind, "<what>: cannot write
/// '<path>': <reason>"), \p what naming the file's role (a flag).
llvm::LogicalResult writeFileAtomic(llvm::StringRef path, WriteBody body,
                                    llvm::StringRef kind, llvm::StringRef what,
                                    std::optional<mlir::Location> loc = {});

} // namespace sea

#endif // SEA_SUPPORT_WRITEFILE_H
