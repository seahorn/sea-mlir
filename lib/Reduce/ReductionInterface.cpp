// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ReductionInterface.cpp - sea DialectReductionPatternInterface -*- C++
//-*-===//
//
// The sea dialect's reduction patterns for mlir-reduce's `reduction-tree`
// pass: every pattern of ReductionPatterns.cpp. reduction-tree applies them
// to the ops outside the range of the variant it tries, then runs the
// interestingness test (a candidate that does not verify is uninteresting
// without running it). Attached as a dialect extension so the dialect
// library does not depend on MLIRReduce.
//
//===----------------------------------------------------------------------===//

#include "sea/Dialect/Sea/SeaDialect.h"
#include "sea/Reduce/Reduce.h"

#include "mlir/IR/DialectRegistry.h"
#include "mlir/Reducer/ReductionPatternInterface.h"

#include <string>
#include <vector>

using namespace mlir;

namespace {
struct SeaReductionPatternInterface : DialectReductionPatternInterface {
  SeaReductionPatternInterface(Dialect *dialect)
      : DialectReductionPatternInterface(dialect) {}
  void populateReductionPatterns(RewritePatternSet &patterns) const final {
    (void)sea::populateSeaReductionPatterns(patterns,
                                            sea::reductionTreePatterns());
  }
};
} // namespace

static std::vector<std::string> &treePatterns() {
  static std::vector<std::string> names;
  return names;
}

std::vector<std::string> &sea::reductionTreePatterns() {
  return treePatterns();
}

void sea::registerSeaReductionInterface(DialectRegistry &registry) {
  registry.addExtension(+[](MLIRContext *, sea::SeaDialect *dialect) {
    dialect->addInterfaces<SeaReductionPatternInterface>();
  });
}
