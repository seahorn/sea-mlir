// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Diag.cpp - visible, non-flooding warnings ----------------*- C++ -*-===//

#include "sea/Support/Diag.h"
#include "sea/Support/Log.h"

#include "llvm/ADT/StringMap.h"

#include <cstdlib>
#include <mutex>
#include <string>

using namespace llvm;

namespace sea::diag {

namespace {
struct Entry {
  unsigned count = 0;
  bool isError = false;
  std::string first; // location of the first occurrence
};

StringMap<Entry> &registry() {
  static StringMap<Entry> entries;
  return entries;
}

/// MLIR runs passes on different programs in parallel: every access to the
/// registry and to stderr goes through this lock.
std::mutex &lock() {
  static std::mutex m;
  return m;
}

std::string locString(std::optional<mlir::Location> loc) {
  if (!loc)
    return "";
  std::string s;
  raw_string_ostream os(s);
  os << *loc;
  return s;
}

/// Colors only on a terminal, and never with NO_COLOR set.
bool useColor(raw_ostream &os) {
  return os.has_colors() && !std::getenv("NO_COLOR");
}

bool logEnabled(StringRef kind) { return log::warningSelected(kind); }

void print(raw_ostream::Colors color, StringRef prefix, StringRef kind,
           const Twine &msg, const std::string &where) {
  raw_ostream &os = errs();
  bool c = useColor(os);
  if (c)
    os.changeColor(color, /*bold=*/true);
  os << prefix;
  if (c)
    os.resetColor();
  os << msg;
  if (!where.empty())
    os << " at " << where;
  os << " [" << kind << "]\n";
  os.flush();
}
} // namespace

void warn(StringRef kind, const Twine &msg, std::optional<mlir::Location> loc) {
  std::lock_guard<std::mutex> guard(lock());
  Entry &e = registry()[kind];
  if (++e.count == 1) {
    e.first = locString(loc);
    print(raw_ostream::YELLOW, "Warning: ", kind, msg, e.first);
  } else if (logEnabled(kind)) {
    print(raw_ostream::YELLOW, "Warning: ", kind, msg, locString(loc));
  }
}

void error(StringRef kind, const Twine &msg,
           std::optional<mlir::Location> loc) {
  std::lock_guard<std::mutex> guard(lock());
  Entry &e = registry()[kind];
  e.isError = true;
  if (++e.count == 1)
    e.first = locString(loc);
  print(raw_ostream::RED, "Error: ", kind, msg, locString(loc));
}

unsigned count(StringRef kind) {
  std::lock_guard<std::mutex> guard(lock());
  auto it = registry().find(kind);
  return it == registry().end() ? 0 : it->second.count;
}

void printSummary(raw_ostream &os) {
  std::lock_guard<std::mutex> guard(lock());
  // Sorted, so the summary is deterministic.
  SmallVector<StringRef> kinds;
  for (auto &kv : registry())
    if (!kv.second.isError && kv.second.count > 1)
      kinds.push_back(kv.first());
  llvm::sort(kinds);
  for (StringRef k : kinds) {
    const Entry &e = registry()[k];
    bool c = useColor(os);
    if (c)
      os.changeColor(raw_ostream::YELLOW, /*bold=*/true);
    os << "Warning: ";
    if (c)
      os.resetColor();
    os << "[" << k << "] occurred " << e.count << " times";
    if (!e.first.empty())
      os << " (first at " << e.first << ")";
    os << "; --log=warn." << k << ":2 shows each\n";
  }
  os.flush();
}

} // namespace sea::diag
