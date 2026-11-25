// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- InstallLayout.cpp - data files found from the executable -----------===//

#include "sea/Support/InstallLayout.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace llvm;

namespace sea {

std::string mainExecutableRealPath(const char *argv0) {
  std::string exe = sys::fs::getMainExecutable(
      argv0, reinterpret_cast<void *>(&mainExecutableRealPath));
  if (exe.empty())
    return exe;
  SmallString<256> real;
  if (sys::fs::real_path(exe, real))
    return exe;
  return std::string(real);
}

SmallString<256> joinClean(StringRef dir, StringRef a, StringRef b, StringRef c,
                           StringRef d) {
  SmallString<256> p(dir);
  sys::path::append(p, a, b, c, d);
  sys::path::remove_dots(p, /*remove_dot_dot=*/true);
  return p;
}

std::string installShareDir(StringRef exePath, StringRef sub) {
  SmallString<256> p =
      joinClean(sys::path::parent_path(exePath), "..", "share");
  sys::path::append(p, sub);
  return std::string(p);
}

std::string buildSourceTree(StringRef exePath) {
  auto buf = MemoryBuffer::getFile(
      joinClean(sys::path::parent_path(exePath), "..", "CMakeCache.txt"),
      /*IsText=*/true);
  if (!buf)
    return "";
  SmallVector<StringRef> lines;
  (*buf)->getBuffer().split(lines, '\n');
  for (StringRef line : lines) {
    line = line.rtrim("\r");
    if (line.consume_front("sea-mlir_SOURCE_DIR:"))
      return line.split('=').second.str();
  }
  return "";
}

} // namespace sea
