// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaImpOps.cpp - seaimp dialect operations ----------------*- C++ -*-===//
//
// The verifier rules and the Mem2Reg interfaces of seaimp.var / get / set
// (docs/design/SeaImp.md). Upstream Mem2Reg (tryToPromoteMemorySlots)
// refuses a slot silently; the verifier admits only variables it promotes:
// the handle is used only as the variable operand of get / set, in the
// variable's own region (a CFG region of sea.br / sea.cond_br, which are
// BranchOpInterface), and the variable is in the entry block (its default
// value, made at the start of that block, dominates every use).
//
//===----------------------------------------------------------------------===//

#include "sea/Dialect/SeaImp/SeaImpOps.h"
#include "sea/Dialect/Sea/SeaOps.h"

#include "mlir/IR/Builders.h"

using namespace mlir;
using namespace sea::imp;

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
    bool ok = (isa<GetOp>(user) && use.getOperandNumber() == 0) ||
              (isa<SetOp>(user) && use.getOperandNumber() == 0);
    if (!ok) {
      InFlightDiagnostic diag =
          emitOpError("the variable handle may be used only as the variable "
                      "operand of seaimp.get / seaimp.set");
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
    return sea::PtrNondetOp::create(builder, loc, t, getNameAttr(),
                                    getSiteAttr());
  // VarType::verify admits only the types above
  llvm_unreachable("seaimp.var of a type without a nondet initial value");
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
