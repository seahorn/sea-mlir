// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaImpOps.cpp - seaimp dialect operations ----------------*- C++ -*-===//
//
// The verifier rules and the Mem2Reg interfaces of seaimp.var / get / set,
// and the verifiers of the region accesses (docs/design/SeaImp.md).
// Upstream Mem2Reg (tryToPromoteMemorySlots) refuses a slot silently; the
// verifier admits only variables it promotes: the handle is used only as
// the variable operand of seaimp ops, in the variable's own region (a CFG
// region of sea.br / sea.cond_br, which are BranchOpInterface), and the
// variable is in the entry block (its default value, made at the start of
// that block, dominates every use). The region accesses are expanded into
// get / sea op / set before Mem2Reg runs (sea-lower-seaimp), so Mem2Reg
// sees only get and set.
//
//===----------------------------------------------------------------------===//

#include "sea/Dialect/SeaImp/SeaImpOps.h"
#include "sea/Dialect/Sea/SeaOps.h"

#include "mlir/IR/Builders.h"

using namespace mlir;
using namespace sea::imp;

//===----------------------------------------------------------------------===//
// Region types
//===----------------------------------------------------------------------===//

static sea::MemType regionOf(Type varType) {
  if (auto v = dyn_cast<VarType>(varType))
    return dyn_cast<sea::MemType>(v.getValueType());
  return {};
}

Type sea::imp::regionIndexType(Type varType) {
  if (sea::MemType m = regionOf(varType))
    return m.getIndex();
  return NoneType::get(varType.getContext());
}

Type sea::imp::regionLenType(Type varType) {
  if (sea::MemType m = regionOf(varType))
    return IntegerType::get(varType.getContext(), m.getIndexWidth());
  return NoneType::get(varType.getContext());
}

Type sea::imp::regionUnitType(Type varType) {
  if (sea::MemType m = regionOf(varType))
    return m.getUnitType();
  return NoneType::get(varType.getContext());
}

#define GET_OP_CLASSES
#include "sea/Dialect/SeaImp/SeaImpOps.cpp.inc"

//===----------------------------------------------------------------------===//
// VarOp
//===----------------------------------------------------------------------===//

LogicalResult VarOp::verify() {
  if (!isa_and_nonnull<sea::ProgramOp>((*this)->getParentOp()))
    return emitOpError("must be in a sea.program");
  if (!(*this)->getBlock()->isEntryBlock())
    return emitOpError("must be in the entry block of its sea.program");
  for (OpOperand &use : getResult().getUses()) {
    Operation *user = use.getOwner();
    // every operand of a seaimp access that may hold a handle is a
    // variable operand (the others are typed as sea values), except set's
    // stored value
    bool ok =
        isa<GetOp, LoadOp, StoreOp, MemSetOp, MemMoveOp, MemHavocOp, AllocaOp>(
            user) ||
        (isa<SetOp>(user) && use.getOperandNumber() == 0);
    if (!ok) {
      InFlightDiagnostic diag =
          emitOpError("the variable handle may be used only as the variable "
                      "operand of seaimp ops");
      diag.attachNote(user->getLoc()) << "used here by " << user->getName();
      return diag;
    }
  }
  return success();
}

SmallVector<MemorySlot> VarOp::getPromotableSlots() {
  return {MemorySlot{getResult(), getValueType()}};
}

Value VarOp::getDefaultValue(const MemorySlot &slot, OpBuilder &builder) {
  Location loc = getLoc();
  Type t = slot.valueType;
  if (isa<IntegerType>(t))
    return sea::NondetOp::create(builder, loc, t, getNameAttr(), getSiteAttr());
  if (isa<sea::StructType>(t))
    return sea::StructNondetOp::create(builder, loc, t, getNameAttr(),
                                       getSiteAttr());
  if (isa<sea::PtrType>(t))
    return sea::PtrNullOp::create(builder, loc, t);
  if (isa<sea::MemType>(t))
    return sea::MemNondetOp::create(builder, loc, t, getNameAttr(),
                                    getSiteAttr());
  // VarType::verify admits only the types above
  llvm_unreachable("seaimp.var of a type without a initial value");
}

void VarOp::handleBlockArgument(const MemorySlot &slot, BlockArgument argument,
                                OpBuilder &builder) {}

std::optional<PromotableAllocationOpInterface>
VarOp::handlePromotionComplete(const MemorySlot &slot, Value defaultValue,
                               OpBuilder &builder) {
  if (defaultValue && defaultValue.use_empty())
    defaultValue.getDefiningOp()->erase();
  this->erase();
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// GetOp / SetOp
//===----------------------------------------------------------------------===//

static LogicalResult verifyAccess(Operation *op, Value var) {
  auto decl = var.getDefiningOp<VarOp>();
  if (!decl)
    return op->emitOpError(
        "the variable operand must be the result of a seaimp.var");
  if (decl->getParentRegion() != op->getParentRegion())
    return op->emitOpError("must be in the region of its seaimp.var (not in "
                           "a nested region)");
  return success();
}

LogicalResult GetOp::verify() { return verifyAccess(*this, getVar()); }
LogicalResult SetOp::verify() { return verifyAccess(*this, getVar()); }

bool GetOp::loadsFrom(const MemorySlot &slot) { return getVar() == slot.ptr; }

bool GetOp::storesTo(const MemorySlot &slot) { return false; }

Value GetOp::getStored(const MemorySlot &slot, OpBuilder &builder,
                       Value reachingDef, const DataLayout &dataLayout) {
  llvm_unreachable("getStored called on seaimp.get");
}

bool GetOp::canUsesBeRemoved(const MemorySlot &slot,
                             const SmallPtrSetImpl<OpOperand *> &blockingUses,
                             SmallVectorImpl<OpOperand *> &newBlockingUses,
                             const DataLayout &dataLayout) {
  if (blockingUses.size() != 1)
    return false;
  Value blockingUse = (*blockingUses.begin())->get();
  return blockingUse == slot.ptr && getVar() == slot.ptr &&
         getResult().getType() == slot.valueType;
}

DeletionKind
GetOp::removeBlockingUses(const MemorySlot &slot,
                          const SmallPtrSetImpl<OpOperand *> &blockingUses,
                          OpBuilder &builder, Value reachingDefinition,
                          const DataLayout &dataLayout) {
  getResult().replaceAllUsesWith(reachingDefinition);
  return DeletionKind::Delete;
}

bool SetOp::loadsFrom(const MemorySlot &slot) { return false; }

bool SetOp::storesTo(const MemorySlot &slot) { return getVar() == slot.ptr; }

Value SetOp::getStored(const MemorySlot &slot, OpBuilder &builder,
                       Value reachingDef, const DataLayout &dataLayout) {
  return getValue();
}

bool SetOp::canUsesBeRemoved(const MemorySlot &slot,
                             const SmallPtrSetImpl<OpOperand *> &blockingUses,
                             SmallVectorImpl<OpOperand *> &newBlockingUses,
                             const DataLayout &dataLayout) {
  if (blockingUses.size() != 1)
    return false;
  Value blockingUse = (*blockingUses.begin())->get();
  return blockingUse == slot.ptr && getVar() == slot.ptr &&
         getValue() != slot.ptr && getValue().getType() == slot.valueType;
}

DeletionKind
SetOp::removeBlockingUses(const MemorySlot &slot,
                          const SmallPtrSetImpl<OpOperand *> &blockingUses,
                          OpBuilder &builder, Value reachingDefinition,
                          const DataLayout &dataLayout) {
  return DeletionKind::Delete;
}

//===----------------------------------------------------------------------===//
// Region accesses
//===----------------------------------------------------------------------===//

/// `align` (bytes, as LLVM's) is a power of two.
static LogicalResult verifyAlign(Operation *op, std::optional<uint64_t> align,
                                 StringRef name = "align") {
  if (align && !llvm::isPowerOf2_64(*align))
    return op->emitOpError()
           << name << " must be a power of two, got " << *align;
  return success();
}

/// A value loaded or stored spans whole cells (as sea.load / sea.store).
static LogicalResult verifyCellValue(Operation *op, Value var, Type t,
                                     StringRef what) {
  sea::MemType mem = regionOf(var.getType());
  unsigned cell = mem.getCell().getWidth();
  if (sea::bitWidthOf(t) % cell == 0)
    return success();
  return op->emitOpError() << what << " type " << t << " does not fit cells "
                           << mem.getCell();
}

LogicalResult LoadOp::verify() {
  if (failed(verifyAccess(*this, getVar())) ||
      failed(verifyAlign(*this, getAlign())))
    return failure();
  return verifyCellValue(*this, getVar(), getResult().getType(), "result");
}

LogicalResult StoreOp::verify() {
  if (failed(verifyAccess(*this, getVar())) ||
      failed(verifyAlign(*this, getAlign())))
    return failure();
  return verifyCellValue(*this, getVar(), getValue().getType(), "value");
}

LogicalResult MemSetOp::verify() {
  if (failed(verifyAccess(*this, getVar())))
    return failure();
  return verifyAlign(*this, getAlign());
}

LogicalResult MemMoveOp::verify() {
  if (failed(verifyAccess(*this, getVar())) ||
      failed(verifyAccess(*this, getSrc())) ||
      failed(verifyAlign(*this, getAlign())))
    return failure();
  return verifyAlign(*this, getSrcAlign(), "src_align");
}

LogicalResult MemHavocOp::verify() { return verifyAccess(*this, getVar()); }

//===----------------------------------------------------------------------===//
// Allocation in a region
//===----------------------------------------------------------------------===//

LogicalResult AllocaOp::verify() {
  if (failed(verifyAccess(*this, getVar())))
    return failure();
  sea::MemType mem = regionOf(getVar().getType());
  auto ptr = cast<sea::PtrType>(getResult().getType());
  if (ptr != mem.getIndex())
    return emitOpError("result must be the pointer type that indexes the "
                       "region (")
           << mem.getIndex() << "), got " << getResult().getType();
  if (sea::bitWidthOf(getSize().getType()) != ptr.getWidth())
    return emitOpError("size must have the pointer width");
  if (!llvm::isPowerOf2_64(getAlign()))
    return emitOpError("alignment must be a power of 2");
  return success();
}
