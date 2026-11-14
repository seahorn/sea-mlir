// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- PassLog.cpp - lower.pass: one log event per pass run ---------------===//
//
// A PassInstrumentation that emits one `lower.pass` level-1 event per pass
// run: the pass, the operation it ran on and its program, the ops under
// that operation before and after, and the time. The ops are counted only
// when the tag is on. Pass adaptors (the nesting of a pipeline) are not passes
// of their own: their passes get the events.
//
//===----------------------------------------------------------------------===//

#include "sea/Support/Log.h"
#include "sea/Support/Timer.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/PassInstrumentation.h"
#include "mlir/Pass/PassManager.h"

#include <vector>

using namespace mlir;

namespace {

sea::log::Tag logPass("lower.pass", 1,
                      "each pass of a pass manager sea-vc runs (--lower, "
                      "--sink-assumes, --print-refs): ops before and after, "
                      "time");

uint64_t countOps(Operation *op) {
  uint64_t n = 0;
  op->walk([&](Operation *) { ++n; });
  return n;
}

/// The adaptor MLIR inserts to run a nested pipeline (PassDetail.h, not
/// installed): known by its name and its lack of an argument.
bool isAdaptor(Pass *pass) {
  return pass->getArgument().empty() &&
         pass->getName().ends_with("OpToOpPassAdaptor");
}

/// `@<name>` of the program the pass ran on (or that holds the operation),
/// `-` when there is none (the module): events of several programs,
/// lowered in parallel, interleave.
std::string programOf(Operation *op) {
  for (Operation *o = op; o; o = o->getParentOp())
    if (o->getName().getStringRef() == "sea.program")
      if (auto s =
              o->getAttrOfType<StringAttr>(SymbolTable::getSymbolAttrName()))
        return ("@" + s.getValue()).str();
  return "-";
}

class PassLog : public PassInstrumentation {
  struct Start {
    uint64_t ops;
    sea::Stopwatch time;
  };
  /// The runs in progress on this thread: before and after of one pass run
  /// on one thread, properly nested (a pass running a dynamic pipeline)
  static std::vector<Start> &stack() {
    thread_local std::vector<Start> s;
    return s;
  }

  void runBeforePass(Pass *pass, Operation *op) override {
    if (!logPass.on(sea::log::Flow) || isAdaptor(pass))
      return;
    stack().push_back({countOps(op), sea::Stopwatch()});
  }
  void end(Pass *pass, Operation *op, bool failed) {
    if (!logPass.on(sea::log::Flow) || isAdaptor(pass) || stack().empty())
      return;
    Start s = stack().back();
    stack().pop_back();
    double sec = s.time.seconds();
    StringRef arg = pass->getArgument();
    SEA_LOG(logPass, sea::log::Flow)
        .msg(failed ? "failed" : "")
        .kv("pass", arg.empty() ? pass->getName() : arg)
        .kv("on", op->getName().getStringRef())
        .kv("program", programOf(op))
        .kv("ops-before", s.ops)
        .kv("ops-after", countOps(op))
        .kv("time", sec);
  }
  void runAfterPass(Pass *pass, Operation *op) override {
    end(pass, op, false);
  }
  void runAfterPassFailed(Pass *pass, Operation *op) override {
    end(pass, op, true);
  }
};

} // namespace

void sea::addPassLog(PassManager &pm) {
  pm.addInstrumentation(std::make_unique<PassLog>());
}
