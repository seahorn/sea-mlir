// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaDialect.cpp - sea dialect -----------------------------*- C++ -*-===//

#include "sea/Dialect/Sea/SeaDialect.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Dialect/Sea/SeaTypes.h"

#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace sea;

#include "sea/Dialect/Sea/SeaOpsDialect.cpp.inc"

#define GET_TYPEDEF_CLASSES
#include "sea/Dialect/Sea/SeaOpsTypes.cpp.inc"

LogicalResult AddrType::verify(function_ref<InFlightDiagnostic()> emitError,
                               unsigned width, unsigned unit) {
  if (width < 1 || width > 64)
    return emitError() << "address width must be in 1..64, got " << width;
  if (unit < 1)
    return emitError() << "address unit must be at least 1 bit";
  return success();
}

LogicalResult PtrType::verify(function_ref<InFlightDiagnostic()> emitError,
                              unsigned width) {
  if (width < 1 || width > 64)
    return emitError() << "pointer width must be in 1..64, got " << width;
  return success();
}

LogicalResult MemType::verify(function_ref<InFlightDiagnostic()> emitError,
                              Type index, IntegerType cell, MemLayout layout) {
  if (!isa<AddrType, PtrType>(index))
    return emitError() << "memory index must be !sea.addr or !sea.ptr, got "
                       << index;
  if (!cell.isSignless())
    return emitError() << "memory cells must be signless integers";
  // Keyed: a cell at every byte address, of whole bytes, independent.
  if (layout == MemLayout::keyed) {
    if (auto a = dyn_cast<AddrType>(index); a && a.getUnit() != 8)
      return emitError() << "the keyed layout needs byte addresses (unit = 8)";
    if (cell.getWidth() % 8 != 0)
      return emitError() << "a keyed cell is whole bytes, got " << cell;
    return success();
  }
  // Word layouts: k-byte cells (k > 1) in byte-addressed memory.
  if (layout != MemLayout::exact) {
    if (auto a = dyn_cast<AddrType>(index); a && a.getUnit() != 8)
      return emitError() << "word layouts need byte addresses (unit = 8)";
    if (cell.getWidth() % 8 != 0 || cell.getWidth() <= 8)
      return emitError() << "a word layout has cells of k > 1 bytes, got "
                         << cell;
    return success();
  }
  // The exact layout: a cell is one index unit (byte memory, or
  // word-addressed memory of unit-sized words).
  if (isa<PtrType>(index) && cell.getWidth() != 8)
    return emitError() << "pointer-indexed memory has byte cells (i8), got "
                       << cell;
  if (auto a = dyn_cast<AddrType>(index); a && cell.getWidth() != a.getUnit())
    return emitError() << "memory cells must be one address unit ("
                       << a.getUnit() << " bits), got " << cell;
  return success();
}

LogicalResult StructType::verify(function_ref<InFlightDiagnostic()> emitError,
                                 ArrayRef<Type> fields) {
  if (fields.empty())
    return emitError() << "a struct needs at least one field";
  for (Type f : fields)
    if (!isa<IntegerType, AddrType, PtrType, StructType>(f) ||
        (isa<IntegerType>(f) && !cast<IntegerType>(f).isSignless()))
      return emitError() << "struct fields must be signless integers, "
                            "addresses, pointers or structs, got "
                         << f;
  return success();
}

static unsigned numLeaves(Type t) {
  if (auto s = dyn_cast<StructType>(t))
    return s.getNumLeaves();
  return 1;
}

unsigned StructType::getNumLeaves() const {
  unsigned n = 0;
  for (Type f : getFields())
    n += numLeaves(f);
  return n;
}

unsigned StructType::getLeafOffset(unsigned i) const {
  unsigned n = 0;
  for (Type f : getFields().take_front(i))
    n += numLeaves(f);
  return n;
}

unsigned MemType::getIndexWidth() const { return bitWidthOf(getIndex()); }

IntegerType MemType::getUnitType() const {
  return isWordLayout() || isKeyed() ? IntegerType::get(getContext(), 8)
                                     : getCell();
}

unsigned MemType::getUnitCopies() const {
  return getCell().getWidth() / getUnitType().getWidth();
}

unsigned MemType::getCellUnits() const {
  return isWordLayout() ? getCell().getWidth() / 8 : 1;
}

unsigned sea::bitWidthOf(Type t) {
  if (auto a = dyn_cast<AddrType>(t))
    return a.getWidth();
  if (auto p = dyn_cast<PtrType>(t))
    return p.getWidth();
  return t.getIntOrFloatBitWidth();
}

void SeaDialect::registerTypes() {
  addTypes<
#define GET_TYPEDEF_LIST
#include "sea/Dialect/Sea/SeaOpsTypes.cpp.inc"
      >();
}

void SeaDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "sea/Dialect/Sea/SeaOps.cpp.inc"
      >();
  registerTypes();
}
