// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ParseInput.cpp - parse a program, keeping positions of names -------===//
//
// See ParseInput.h.
//
//===----------------------------------------------------------------------===//

#include "sea/Interp/ParseInput.h"

#include "sea/Interp/Interpreter.h"

#include "mlir/AsmParser/AsmParser.h"
#include "mlir/AsmParser/AsmParserState.h"
#include "mlir/Bytecode/BytecodeReader.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

using namespace mlir;

namespace sea {

/// The position `at` of `sm`'s main buffer.
static FileLineColLoc positionOf(MLIRContext *ctx, const llvm::SourceMgr &sm,
                                 llvm::SMLoc at) {
  auto [line, col] = sm.getLineAndColumn(at);
  StringRef file =
      sm.getMemoryBuffer(sm.getMainFileID())->getBufferIdentifier();
  return FileLineColLoc::get(ctx, file, line, col);
}

/// `loc` with the position `at` of `sm`'s main buffer where it has none:
/// in place of the unknown location at the bottom of a chain of names
/// (loc("a"), loc("a"("b"))), and added to a fused location without a
/// file position (loc(fused<"llvm @f %bb #3">[]), whose metadata is not a
/// position; its elements first get theirs: fused<"m">["a"] ->
/// fused<"m">["a"(pos)]); else `loc`.
static Location withPosition(Location loc, const llvm::SourceMgr &sm,
                             llvm::SMLoc at) {
  if (!at.isValid())
    return loc;
  if (auto f = dyn_cast<FusedLoc>(loc)) {
    if (fileLocOf(loc))
      return loc;
    SmallVector<Location> locs;
    for (Location l : f.getLocations())
      locs.push_back(withPosition(l, sm, at));
    Location r = FusedLoc::get(locs, f.getMetadata(), loc.getContext());
    if (fileLocOf(r))
      return r;
    locs.push_back(positionOf(loc.getContext(), sm, at));
    return FusedLoc::get(locs, f.getMetadata(), loc.getContext());
  }
  auto n = dyn_cast<NameLoc>(loc);
  if (!n)
    return loc;
  Location child = n.getChildLoc();
  if (isa<UnknownLoc>(child))
    child = positionOf(loc.getContext(), sm, at);
  else
    child = withPosition(child, sm, at);
  return child == n.getChildLoc() ? loc : NameLoc::get(n.getName(), child);
}

OwningOpRef<ModuleOp> parseInputFile(StringRef filename, MLIRContext *context) {
  std::string error;
  std::unique_ptr<llvm::MemoryBuffer> buf = openInputFile(filename, &error);
  if (!buf) {
    emitError(UnknownLoc::get(context)) << error;
    return nullptr;
  }
  ParserConfig config(context);
  llvm::SourceMgr sm;
  sm.AddNewSourceBuffer(std::move(buf), llvm::SMLoc());
  // bytecode carries no source positions: the plain parser
  if (isBytecode(*sm.getMemoryBuffer(sm.getMainFileID())))
    return parseSourceFile<ModuleOp>(sm, config);
  Location fileLoc = FileLineColLoc::get(
      context, sm.getMemoryBuffer(sm.getMainFileID())->getBufferIdentifier(), 0,
      0);
  Block block;
  AsmParserState state;
  if (failed(parseAsmSourceFile(sm, &block, config, &state)))
    return nullptr;
  for (const AsmParserState::OperationDefinition &def : state.getOpDefs())
    def.op->setLoc(withPosition(def.op->getLoc(), sm, def.loc.Start));
  for (const AsmParserState::BlockDefinition &def : state.getBlockDefs())
    for (auto [arg, adef] : llvm::zip(def.block->getArguments(), def.arguments))
      arg.setLoc(withPosition(arg.getLoc(), sm, adef.loc.Start));
  return mlir::detail::constructContainerOpForParserIfNecessary<ModuleOp>(
      &block, context, fileLoc);
}

} // namespace sea
