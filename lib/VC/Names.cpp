// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Names.cpp - solver names for values and blocks -----------*- C++ -*-===//

#include "sea/VC/Names.h"

#include "llvm/Support/raw_ostream.h"

using namespace mlir;

namespace sea {

ValueNames::ValueNames(ProgramOp program)
    : ctx(program.getContext()),
      state(program->getParentOp() ? program->getParentOp()
                                   : program.getOperation(),
            OpPrintingFlags().printNameLocAsPrefix()) {}

StringAttr ValueNames::name(Value v) {
  auto it = values.find(v);
  if (it != values.end())
    return it->second;
  std::string text;
  llvm::raw_string_ostream os(text);
  if (auto nd = v.getDefiningOp<NondetOp>())
    os << "nd." << nd.getSite();
  else if (auto mn = v.getDefiningOp<MemNondetOp>())
    os << "nd." << mn.getSite();
  else
    v.printAsOperand(os, state);
  return values[v] = StringAttr::get(ctx, text);
}

StringAttr ValueNames::name(Block *b) {
  auto it = blocks.find(b);
  if (it != blocks.end())
    return it->second;
  std::string text;
  llvm::raw_string_ostream os(text);
  b->printAsOperand(os, state);
  return blocks[b] = StringAttr::get(ctx, text);
}

} // namespace sea
