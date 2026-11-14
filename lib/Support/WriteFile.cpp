// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- WriteFile.cpp - output files written whole -------------------------===//

#include "sea/Support/WriteFile.h"

#include "sea/Support/Diag.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace llvm;

LogicalResult sea::writeFileAtomic(StringRef path, WriteBody body,
                                   std::string &error) {
  error.clear();
  SmallString<128> tmp = sys::path::parent_path(path);
  sys::path::append(tmp, "." + sys::path::filename(path).str() + ".tmp");
  auto cannot = [&](const std::error_code &ec) {
    error = "cannot write '" + path.str() + "': " + ec.message();
  };
  {
    std::error_code ec;
    raw_fd_ostream os(tmp, ec, sys::fs::OF_Text);
    if (ec) {
      cannot(ec);
      return failure();
    }
    if (failed(body(os))) {
      os.close();
      os.clear_error();
      sys::fs::remove(tmp);
      return failure();
    }
    os.close();
    if (os.has_error()) {
      cannot(os.error());
      os.clear_error();
      sys::fs::remove(tmp);
      return failure();
    }
  }
  if (std::error_code ec = sys::fs::rename(tmp, path)) {
    error = "cannot rename '" + tmp.str().str() + "' to '" + path.str() +
            "': " + ec.message();
    sys::fs::remove(tmp);
    return failure();
  }
  return success();
}

LogicalResult sea::writeFileAtomic(StringRef path, WriteBody body,
                                   StringRef kind, StringRef what,
                                   std::optional<mlir::Location> loc) {
  std::string error;
  if (succeeded(writeFileAtomic(path, body, error)))
    return success();
  if (!error.empty())
    diag::error(kind, what + ": " + error, loc);
  return failure();
}
