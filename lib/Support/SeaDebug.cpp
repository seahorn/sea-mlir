// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sea/Support/SeaDebug.h"
#include "llvm/Support/CommandLine.h"

#include <set>
#include <string>

#ifndef NSEALOG
using namespace sea;
bool sea::SeaLogFlag = false;
std::set<std::string> sea::SeaLog;

void sea::SeaEnableLog(std::string x) {
  if (x.empty())
    return;
  SeaLogFlag = true;
  SeaLog.insert(x);
}

namespace sea {
struct LogOpt {
  void operator=(const std::string &tag) const { sea::SeaEnableLog(tag); }
};

LogOpt loc;
} // namespace sea

static llvm::cl::opt<sea::LogOpt, true, llvm::cl::parser<std::string>>
    LogClOption("log", llvm::cl::desc("Enable specified log level"),
                llvm::cl::location(sea::loc), llvm::cl::value_desc("string"),
                llvm::cl::ValueRequired, llvm::cl::ZeroOrMore);
#endif
