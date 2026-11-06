// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SeaOps.cpp - sea dialect operations ----------------------*- C++ -*-===//

#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Dialect/Sea/Semantics.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "llvm/ADT/DenseMap.h"

using namespace mlir;
using namespace sea;

#include "sea/Dialect/Sea/SeaEnums.cpp.inc"

#define GET_OP_CLASSES
#include "sea/Dialect/Sea/SeaOps.cpp.inc"

//===----------------------------------------------------------------------===//
// ProgramOp
//===----------------------------------------------------------------------===//

LogicalResult ProgramOp::verifyRegions() {
  Region &body = getBody();
  if (body.empty())
    return emitOpError("must have at least one block");
  if (body.front().getNumArguments() != 0)
    return emitOpError("entry block must have no arguments");

  // Nondet sites name the program's inputs (scalars, initial memories,
  // havocked ranges); they must be unique.
  llvm::DenseMap<uint64_t, Operation *> sites;
  WalkResult res = body.walk([&](Operation *op) {
    std::optional<uint64_t> site;
    if (auto nd = dyn_cast<NondetOp>(op))
      site = nd.getSite();
    else if (auto sn = dyn_cast<StructNondetOp>(op))
      site = sn.getSite();
    else if (auto mn = dyn_cast<MemNondetOp>(op))
      site = mn.getSite();
    else if (auto mh = dyn_cast<MemHavocOp>(op))
      site = mh.getSite();
    else if (auto al = dyn_cast<AllocaOp>(op))
      site = al.getSite();
    else if (auto ml = dyn_cast<MallocOp>(op))
      site = ml.getSite();
    else if (auto pn = dyn_cast<PtrNondetOp>(op))
      site = pn.getSite();
    if (!site)
      return WalkResult::advance();
    auto [it, inserted] = sites.try_emplace(*site, op);
    if (inserted)
      return WalkResult::advance();
    InFlightDiagnostic diag = op->emitOpError("duplicate nondet site ")
                              << *site;
    diag.attachNote(it->second->getLoc()) << "previous use here";
    return WalkResult::interrupt();
  });
  if (res.wasInterrupted())
    return failure();

  // All uses of a global denote one object: size and alignment must agree.
  llvm::DenseMap<StringAttr, GlobalOp> globals;
  res = body.walk([&](GlobalOp g) {
    auto [it, inserted] = globals.try_emplace(g.getNameAttr().getAttr(), g);
    if (inserted || (it->second.getSize() == g.getSize() &&
                     it->second.getAlign() == g.getAlign()))
      return WalkResult::advance();
    InFlightDiagnostic diag =
        g.emitOpError("size or alignment differs from another use of ")
        << g.getNameAttr();
    diag.attachNote(it->second.getLoc()) << "other use here";
    return WalkResult::interrupt();
  });
  return failure(res.wasInterrupted());
}

//===----------------------------------------------------------------------===//
// Branches
//===----------------------------------------------------------------------===//

SuccessorOperands BrOp::getSuccessorOperands(unsigned index) {
  assert(index == 0 && "invalid successor index");
  return SuccessorOperands(getDestOperandsMutable());
}

SuccessorOperands CondBrOp::getSuccessorOperands(unsigned index) {
  assert(index < getNumSuccessors() && "invalid successor index");
  return SuccessorOperands(index == 0 ? getTrueDestOperandsMutable()
                                      : getFalseDestOperandsMutable());
}

Block *CondBrOp::getSuccessorForOperands(ArrayRef<Attribute> operands) {
  if (auto condition = dyn_cast_or_null<IntegerAttr>(operands.front()))
    return condition.getValue().isOne() ? getTrueDest() : getFalseDest();
  return nullptr;
}

namespace {
// The constant and identical-successor cases of upstream cf.cond_br.
struct SimplifyCondBr : OpRewritePattern<CondBrOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(CondBrOp op,
                                PatternRewriter &rewriter) const override {
    if (matchPattern(op.getCondition(), m_One())) {
      rewriter.replaceOpWithNewOp<BrOp>(op, op.getTrueDestOperands(),
                                        op.getTrueDest());
      return success();
    }
    if (matchPattern(op.getCondition(), m_Zero())) {
      rewriter.replaceOpWithNewOp<BrOp>(op, op.getFalseDestOperands(),
                                        op.getFalseDest());
      return success();
    }
    if (op.getTrueDest() == op.getFalseDest() &&
        op.getTrueDestOperands() == op.getFalseDestOperands()) {
      rewriter.replaceOpWithNewOp<BrOp>(op, op.getTrueDestOperands(),
                                        op.getTrueDest());
      return success();
    }
    return failure();
  }
};
} // namespace

void CondBrOp::getCanonicalizationPatterns(RewritePatternSet &patterns,
                                           MLIRContext *context) {
  patterns.add<SimplifyCondBr>(context);
}

//===----------------------------------------------------------------------===//
// ConstOp
//===----------------------------------------------------------------------===//

LogicalResult ConstOp::verify() {
  if (!llvm::isa<IntegerAttr>(getValue()))
    return emitOpError("value must be an integer attribute");
  return success();
}

//===----------------------------------------------------------------------===//
// Casts
//===----------------------------------------------------------------------===//

static unsigned widthOf(Value v) { return v.getType().getIntOrFloatBitWidth(); }

LogicalResult ZExtOp::verify() {
  if (widthOf(getResult()) <= widthOf(getInput()))
    return emitOpError("result must be wider than the input");
  return success();
}

LogicalResult SExtOp::verify() {
  if (widthOf(getResult()) <= widthOf(getInput()))
    return emitOpError("result must be wider than the input");
  return success();
}

LogicalResult ConcatOp::verify() {
  if (widthOf(getResult()) != widthOf(getHi()) + widthOf(getLo()))
    return emitOpError("result width must be the sum of the operand widths");
  return success();
}

static unsigned leafBits(Type t) {
  if (auto s = dyn_cast<StructType>(t)) {
    unsigned n = 0;
    for (Type f : s.getFields())
      n += leafBits(f);
    return n;
  }
  return isa<PtrType>(t) ? 0 : bitWidthOf(t);
}

unsigned StructNondetOp::getBitsWidth() { return leafBits(getType()); }

LogicalResult StructCreateOp::verify() {
  ArrayRef<Type> fields = getResult().getType().getFields();
  if (fields != ArrayRef<Type>(llvm::to_vector(getFields().getTypes())))
    return emitOpError("operand types must be the struct's field types");
  return success();
}

/// The type of field `index` of `s`, or null if out of range.
static Type fieldType(StructType s, uint64_t index) {
  return index < s.getFields().size() ? s.getFields()[index] : Type();
}

LogicalResult StructExtractOp::verify() {
  Type f = fieldType(getInput().getType(), getIndex());
  if (!f)
    return emitOpError("field index out of range");
  if (f != getResult().getType())
    return emitOpError("result type must be the field's type");
  return success();
}

LogicalResult StructInsertOp::verify() {
  Type f = fieldType(getInput().getType(), getIndex());
  if (!f)
    return emitOpError("field index out of range");
  if (f != getValue().getType())
    return emitOpError("value type must be the field's type");
  return success();
}

LogicalResult BSwapOp::verify() {
  if (widthOf(getResult()) % 16 != 0)
    return emitOpError("width must be a multiple of 16");
  return success();
}

LogicalResult ExtractOp::verify() {
  if (getLow() + widthOf(getResult()) > widthOf(getInput()))
    return emitOpError("extracted bits exceed the input");
  return success();
}

LogicalResult TruncOp::verify() {
  if (widthOf(getResult()) >= widthOf(getInput()))
    return emitOpError("result must be narrower than the input");
  return success();
}

//===----------------------------------------------------------------------===//
// Addresses
//===----------------------------------------------------------------------===//

LogicalResult AddrAddOp::verify() {
  if (bitWidthOf(getOffset().getType()) != getAddr().getType().getWidth())
    return emitOpError("offset must have the address width");
  return success();
}

LogicalResult AddrDiffOp::verify() {
  if (bitWidthOf(getResult().getType()) !=
      cast<AddrType>(getLhs().getType()).getWidth())
    return emitOpError("result must have the address width");
  return success();
}

LogicalResult AddrToIntOp::verify() {
  if (bitWidthOf(getResult().getType()) != getAddr().getType().getWidth())
    return emitOpError("result must have the address width");
  return success();
}

LogicalResult AddrFromIntOp::verify() {
  if (bitWidthOf(getValue().getType()) != getResult().getType().getWidth())
    return emitOpError("value must have the address width");
  return success();
}

LogicalResult AddrToBitsOp::verify() {
  if (bitWidthOf(getResult().getType()) != getAddr().getType().getWidth())
    return emitOpError("result must have the address width");
  return success();
}

LogicalResult AddrFromBitsOp::verify() {
  if (bitWidthOf(getValue().getType()) != getResult().getType().getWidth())
    return emitOpError("value must have the address width");
  return success();
}

LogicalResult SegmentOp::verify() {
  unsigned w = getResult().getType().getWidth();
  if (getBase().getBitWidth() != w || getSize().getBitWidth() != w)
    return emitOpError("base and size must have the address width");
  // The object must not wrap the address space: base + size <= 2^w. Object
  // disjointness (BaseRegistry::overlapping) compares linear intervals, which
  // agrees with the circular address semantics only for such objects; a
  // wrapping object aliases low addresses and made SeaM's cross-object skip
  // unsound. The regression is covered by test/VC/seam/segments-wrap.mlir.
  APInt end = getBase().zext(w + 1) + getSize().zext(w + 1);
  if (end.ugt(APInt::getOneBitSet(w + 1, w)))
    return emitOpError("the object wraps the address space (base + size > "
                       "2^")
           << w << ")";
  return success();
}

//===----------------------------------------------------------------------===//
// Memory
//===----------------------------------------------------------------------===//

/// Checks that `addr` indexes `mem` and that `len` (if any) has the address
/// width.
static LogicalResult verifyAccess(Operation *op, MemType mem, Value addr,
                                  Value len = nullptr) {
  if (addr.getType() != mem.getIndex())
    return op->emitOpError("index type ")
           << addr.getType() << " does not index " << mem;
  if (len && bitWidthOf(len.getType()) != mem.getIndexWidth())
    return op->emitOpError("length must have the index width");
  return success();
}

/// A value loaded or stored: an integer, address or pointer whose width is a
/// positive multiple of the cell width (it spans that many consecutive
/// cells, little-endian).
static LogicalResult verifyCellValue(Operation *op, MemType mem, Type t,
                                     StringRef what) {
  unsigned cell = mem.getCell().getWidth();
  if ((isa<IntegerType, AddrType, PtrType>(t)) && bitWidthOf(t) % cell == 0)
    return success();
  return op->emitOpError() << what << " type " << t << " does not fit cells "
                           << mem.getCell();
}

/// `align` (bytes, as LLVM's) is a power of two.
static LogicalResult verifyAlign(Operation *op, std::optional<uint64_t> align,
                                 StringRef name = "align") {
  if (align && !llvm::isPowerOf2_64(*align))
    return op->emitOpError()
           << name << " must be a power of two, got " << *align;
  return success();
}

LogicalResult LoadOp::verify() {
  if (failed(verifyAlign(*this, getAlign())) ||
      failed(verifyAccess(*this, getMem().getType(), getAddr())))
    return failure();
  return verifyCellValue(*this, getMem().getType(), getResult().getType(),
                         "result");
}

LogicalResult StoreOp::verify() {
  if (failed(verifyAlign(*this, getAlign())) ||
      failed(verifyAccess(*this, getMem().getType(), getAddr())))
    return failure();
  return verifyCellValue(*this, getMem().getType(), getValue().getType(),
                         "value");
}

LogicalResult MemSetOp::verify() {
  if (failed(verifyAlign(*this, getAlign())) ||
      failed(verifyAccess(*this, getMem().getType(), getAddr(), getLen())))
    return failure();
  if (getValue().getType() != getMem().getType().getUnitType())
    return emitOpError("value must be one index unit");
  return success();
}

LogicalResult MemMoveOp::verify() {
  if (getSrc().getType() != getMem().getType())
    return emitOpError("source and destination memories must have the same "
                       "type");
  if (failed(verifyAlign(*this, getAlign())) ||
      failed(verifyAlign(*this, getSrcAlign(), "src_align")) ||
      failed(verifyAccess(*this, getMem().getType(), getAddr(), getLen())))
    return failure();
  return verifyAccess(*this, getSrc().getType(), getSrcAddr());
}

LogicalResult MemHavocOp::verify() {
  return verifyAccess(*this, getMem().getType(), getAddr(), getLen());
}

//===----------------------------------------------------------------------===//
// Abstract pointers and allocation
//===----------------------------------------------------------------------===//

template <typename AllocT>
static LogicalResult verifyAlloc(AllocT op) {
  if (bitWidthOf(op.getSize().getType()) != op.getType().getWidth())
    return op.emitOpError("size must have the pointer width");
  if (!llvm::isPowerOf2_64(op.getAlign()))
    return op.emitOpError("alignment must be a power of 2");
  return success();
}

LogicalResult AllocaOp::verify() { return verifyAlloc(*this); }
LogicalResult MallocOp::verify() { return verifyAlloc(*this); }

LogicalResult PtrAddOp::verify() {
  if (bitWidthOf(getOffset().getType()) != getPtr().getType().getWidth())
    return emitOpError("offset must have the pointer width");
  return success();
}

LogicalResult PtrIndexOp::verify() {
  unsigned n = bitWidthOf(getBase().getType());
  if (getStrides().size() != getIndices().size())
    return emitOpError("needs one stride per index");
  // an n-bit two's complement value, spelled as its 64-bit sign extension
  auto canonical = [&](int64_t v) { return n >= 64 || llvm::isIntN(n, v); };
  for (auto [k, s] : llvm::enumerate(getStrides())) {
    if (s == 0)
      return emitOpError("stride ") << k << " is 0 (drop the index)";
    if (!canonical(s))
      return emitOpError("stride ")
             << k << " does not fit " << n << " signed bits";
  }
  if (!canonical(getOffset()))
    return emitOpError("offset does not fit ") << n << " signed bits";
  for (Value i : getIndices())
    if (bitWidthOf(i.getType()) > n)
      return emitOpError("an index is wider than the pointer (truncate it "
                         "first)");
  return success();
}

LogicalResult PtrDiffOp::verify() {
  if (bitWidthOf(getResult().getType()) != bitWidthOf(getLhs().getType()))
    return emitOpError("result must have the pointer width");
  return success();
}

LogicalResult PtrDereferenceableOp::verify() {
  if (bitWidthOf(getLen().getType()) != getPtr().getType().getWidth())
    return emitOpError("length must have the pointer width");
  return success();
}

LogicalResult PtrToIntOp::verify() {
  if (bitWidthOf(getResult().getType()) != getPtr().getType().getWidth())
    return emitOpError("result must have the pointer width");
  return success();
}

LogicalResult PtrFromIntOp::verify() {
  if (bitWidthOf(getValue().getType()) != getResult().getType().getWidth())
    return emitOpError("value must have the pointer width");
  return success();
}

bool sea::isAbstract(ProgramOp program) {
  std::function<bool(Type)> abstract = [&](Type t) {
    if (auto m = dyn_cast<MemType>(t))
      t = m.getIndex();
    if (auto s = dyn_cast<StructType>(t))
      return llvm::any_of(s.getFields(), abstract);
    return isa<PtrType>(t);
  };
  WalkResult res = program.walk([&](Operation *op) {
    for (Type t : op->getResultTypes())
      if (abstract(t))
        return WalkResult::interrupt();
    for (Region &r : op->getRegions())
      for (Block &b : r)
        for (Type t : b.getArgumentTypes())
          if (abstract(t))
            return WalkResult::interrupt();
    return WalkResult::advance();
  });
  return res.wasInterrupted();
}

//===----------------------------------------------------------------------===//
// Folders. Values come from evalPureOp (the golden semantics); a folder only
// decides whether to fold. None updates its op in place.
//===----------------------------------------------------------------------===//

/// The value of `op` by evalPureOp if every operand is an integer constant.
static OpFoldResult foldByEval(Operation *op, ArrayRef<Attribute> operands) {
  SmallVector<APInt> vals;
  for (Attribute a : operands) {
    auto i = dyn_cast_if_present<IntegerAttr>(a);
    if (!i)
      return {};
    vals.push_back(i.getValue());
  }
  auto type = dyn_cast<IntegerType>(op->getResult(0).getType());
  std::optional<APInt> r = evalPureOp(op, vals);
  if (!type || !r)
    return {};
  return IntegerAttr::get(type, *r);
}

OpFoldResult ConstOp::fold(FoldAdaptor) { return getValue(); }

// nsw / nuw are promises the interpreter checks and sea-wrap-flags turns
// into an assume / assert: a flagged op is not folded, so none is lost.
#define SEA_WRAP_FOLDER(Op)                                                    \
  OpFoldResult Op::fold(FoldAdaptor a) {                                       \
    if (getNsw() || getNuw())                                                  \
      return {};                                                               \
    return foldByEval(*this, a.getOperands());                                 \
  }
SEA_WRAP_FOLDER(AddOp)
SEA_WRAP_FOLDER(SubOp)
SEA_WRAP_FOLDER(MulOp)
SEA_WRAP_FOLDER(ShlOp)
#undef SEA_WRAP_FOLDER

#define SEA_EVAL_FOLDER(Op)                                                    \
  OpFoldResult Op::fold(FoldAdaptor a) {                                       \
    return foldByEval(*this, a.getOperands());                                 \
  }
SEA_EVAL_FOLDER(UDivOp)
SEA_EVAL_FOLDER(SDivOp)
SEA_EVAL_FOLDER(URemOp)
SEA_EVAL_FOLDER(SRemOp)
SEA_EVAL_FOLDER(AndOp)
SEA_EVAL_FOLDER(OrOp)
SEA_EVAL_FOLDER(XorOp)
SEA_EVAL_FOLDER(LShrOp)
SEA_EVAL_FOLDER(AShrOp)
SEA_EVAL_FOLDER(ICmpOp)
SEA_EVAL_FOLDER(ZExtOp)
SEA_EVAL_FOLDER(SExtOp)
SEA_EVAL_FOLDER(TruncOp)
SEA_EVAL_FOLDER(ConcatOp)
SEA_EVAL_FOLDER(ExtractOp)
SEA_EVAL_FOLDER(UMinOp)
SEA_EVAL_FOLDER(UMaxOp)
SEA_EVAL_FOLDER(SMinOp)
SEA_EVAL_FOLDER(SMaxOp)
SEA_EVAL_FOLDER(BSwapOp)
SEA_EVAL_FOLDER(CtPopOp)
SEA_EVAL_FOLDER(FShlOp)
SEA_EVAL_FOLDER(FShrOp)
SEA_EVAL_FOLDER(UAddOOp)
SEA_EVAL_FOLDER(SAddOOp)
SEA_EVAL_FOLDER(USubOOp)
SEA_EVAL_FOLDER(SSubOOp)
SEA_EVAL_FOLDER(UMulOOp)
SEA_EVAL_FOLDER(SMulOOp)
SEA_EVAL_FOLDER(SDivOOp)
SEA_EVAL_FOLDER(NegOOp)
#undef SEA_EVAL_FOLDER

OpFoldResult SelectOp::fold(FoldAdaptor a) {
  if (getTrueValue() == getFalseValue())
    return getTrueValue();
  // A constant condition selects an arm of any type (pointers, structs):
  // which one is evalPureOp's choice between the markers 1 and 0.
  auto c = dyn_cast_if_present<IntegerAttr>(a.getCondition());
  if (!c)
    return {};
  std::optional<APInt> pick =
      evalPureOp(*this, {c.getValue(), APInt(1, 1), APInt(1, 0)});
  if (!pick)
    return {};
  return pick->isOne() ? getTrueValue() : getFalseValue();
}

Operation *SeaDialect::materializeConstant(OpBuilder &b, Attribute value,
                                           Type type, Location loc) {
  auto i = dyn_cast<IntegerAttr>(value);
  auto t = dyn_cast<IntegerType>(type);
  if (!i || !t || !t.isSignless() || i.getType() != t)
    return nullptr;
  return ConstOp::create(b, loc, t, i);
}

//===----------------------------------------------------------------------===//
// Print ops (docs/design/Print.md)
//===----------------------------------------------------------------------===//

namespace sea {

bool isPrintOp(Operation *op) {
  return isa<PrintOp, PrintMemOp, PrintScopeOp, PrintEndOp>(op);
}

Operation *nextNonPrintOp(Operation *op) {
  Operation *n = op->getNextNode();
  while (n && isPrintOp(n))
    n = n->getNextNode();
  return n;
}

/// The name in a NameLoc at the top of `loc` (directly, or one of a fused
/// location's), except the marker sea-merge-asserts puts on branches.
static std::optional<std::string> nameLocName(Location loc) {
  auto of = [](Location l) -> std::optional<std::string> {
    if (auto n = dyn_cast<NameLoc>(l))
      if (n.getName().getValue() != "sea.assert")
        return n.getName().str();
    return std::nullopt;
  };
  if (auto r = of(loc))
    return r;
  if (auto f = dyn_cast<FusedLoc>(loc))
    for (Location l : f.getLocations())
      if (auto r = of(l))
        return r;
  return std::nullopt;
}

SmallVector<std::string, 2> printNamesOf(Value v) {
  SmallVector<std::string, 2> names;
  Location loc = v.getLoc();
  if (Operation *def = v.getDefiningOp()) {
    if (isa<NondetOp, StructNondetOp, PtrNondetOp, MemNondetOp, MemHavocOp>(
            def))
      if (auto site = def->getAttrOfType<IntegerAttr>("site"))
        names.push_back("nd." + std::to_string(site.getInt()));
    if (def->getNumResults() != 1)
      return names;
    loc = def->getLoc();
  }
  if (std::optional<std::string> n = nameLocName(loc))
    names.insert(names.begin(), *n);
  return names;
}

std::optional<std::string> printNameOf(Value v) {
  SmallVector<std::string, 2> names = printNamesOf(v);
  if (names.empty())
    return std::nullopt;
  return names.front();
}

/// The value formats of sea.print / sea.print.mem (and `hexdump`, for
/// print.mem of bytes).
static bool isPrintFormat(StringRef f) {
  return llvm::is_contained(
      ArrayRef<StringRef>{"auto", "u", "s", "x", "b", "c", "bits"}, f);
}

/// A register value type a print can show.
static bool isPrintable(Type t) {
  if (isa<IntegerType>(t))
    return cast<IntegerType>(t).isSignless();
  if (isa<AddrType, PtrType>(t))
    return true;
  if (auto st = dyn_cast<StructType>(t))
    return llvm::all_of(st.getFields(), isPrintable);
  return false;
}

LogicalResult PrintOp::verify() {
  if (getTag().empty())
    return emitOpError("the tag must not be empty");
  if (getRef() && isStrong())
    return emitOpError("a print is weak (a name) or strong (operands), not "
                       "both");
  if (getRef() && !getType())
    return emitOpError("a value name needs its type");
  if (getRef() && getRef()->empty())
    return emitOpError("the value name must not be empty");
  Type t = getShownType();
  if (!getRef() && !isStrong()) {
    if (getType() || getFmt() || getNames())
      return emitOpError("type / fmt / names need a value");
    return success();
  }
  if (isStrong()) {
    if (getType()) {
      // the leaves of a struct (sea-scalarize-structs)
      auto st = dyn_cast<StructType>(*getType());
      if (!st)
        return emitOpError("a strong print with a type shows a struct by "
                           "its leaves");
      if (getArgs().size() != st.getNumLeaves())
        return emitOpError("expected ")
               << st.getNumLeaves() << " leaves, got " << getArgs().size();
    } else if (getArgs().size() != 1) {
      return emitOpError("several operands need the struct type they are the "
                         "leaves of");
    }
  }
  if (!isPrintable(t))
    return emitOpError("cannot print a value of type ") << t;
  if (auto f = getFmt()) {
    if (!isPrintFormat(*f))
      return emitOpError("unknown format '") << *f << "'";
    if (*f != "auto" && !isa<IntegerType, StructType>(t))
      return emitOpError("format '") << *f << "' needs an integer";
  }
  if (auto names = getNames()) {
    auto st = dyn_cast<StructType>(t);
    if (!st)
      return emitOpError("names need a struct type");
    if (names->size() != st.getNumLeaves())
      return emitOpError("expected ")
             << st.getNumLeaves() << " names, got " << names->size();
  }
  return success();
}

// sea.print "tag"
// sea.print "tag" = "name" : type attr-dict
// sea.print "tag" %a, %b : type, type attr-dict
ParseResult PrintOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr tag;
  if (parser.parseAttribute(tag, parser.getBuilder().getNoneType(), "tag",
                            result.attributes))
    return failure();
  if (succeeded(parser.parseOptionalEqual())) {
    StringAttr ref;
    Type t;
    if (parser.parseAttribute(ref, parser.getBuilder().getNoneType(), "ref",
                              result.attributes) ||
        parser.parseColonType(t))
      return failure();
    result.addAttribute("type", TypeAttr::get(t));
  } else {
    SmallVector<OpAsmParser::UnresolvedOperand> ops;
    SmallVector<Type> types;
    if (parser.parseOperandList(ops))
      return failure();
    if (!ops.empty() && (parser.parseColonTypeList(types) ||
                         parser.resolveOperands(ops, types, parser.getNameLoc(),
                                                result.operands)))
      return failure();
  }
  return parser.parseOptionalAttrDict(result.attributes);
}

void PrintOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printAttributeWithoutType(getTagAttr());
  SmallVector<StringRef> elided{"tag"};
  if (getRef()) {
    p << " = ";
    p.printAttributeWithoutType(getRefAttr());
    p << " : " << *getType();
    elided.append({"ref", "type"});
  } else if (isStrong()) {
    p << " " << getArgs() << " : " << getArgs().getTypes();
  }
  p.printOptionalAttrDict((*this)->getAttrs(), elided);
}

LogicalResult PrintMemOp::verify() {
  if (getTag().empty())
    return emitOpError("the tag must not be empty");
  Type t = getType();
  if (auto it = dyn_cast<IntegerType>(t)) {
    if (!it.isSignless() || it.getWidth() % 8 != 0)
      return emitOpError("the element must be whole bytes, got ") << t;
  } else if (!isa<AddrType, PtrType>(t)) {
    return emitOpError("the element must be an integer or a pointer, got ")
           << t;
  }
  if (auto f = getFmt()) {
    if (*f == "hexdump") {
      if (!t.isInteger(8))
        return emitOpError("hexdump reads bytes (type i8)");
    } else if (!isPrintFormat(*f)) {
      return emitOpError("unknown format '") << *f << "'";
    } else if (*f != "auto" && !isa<IntegerType>(t)) {
      return emitOpError("format '") << *f << "' needs an integer";
    }
  }
  if (isStrong()) {
    if (getMem() || getAddr() || getCountRef())
      return emitOpError("a print is weak (names) or strong (operands), not "
                         "both");
    if (getArgs().size() < 2 || getArgs().size() > 3)
      return emitOpError("expected a memory, an address and an optional "
                         "count");
    auto mt = dyn_cast<MemType>(getArgs()[0].getType());
    if (!mt)
      return emitOpError("the first operand must be a memory");
    if (getArgs()[1].getType() != mt.getIndex())
      return emitOpError("the address must index the memory");
    if (getCountValue()) {
      if (!isa<IntegerType>(getCountValue().getType()))
        return emitOpError("the count must be an integer");
      if (getCount())
        return emitOpError("count and a count operand exclude each other");
    }
    return success();
  }
  if (!getMem() || !getAddr() || getMem()->empty() || getAddr()->empty())
    return emitOpError("a weak print.mem needs memory and address names");
  if (getCount() && getCountRef())
    return emitOpError("count and count_ref exclude each other");
  if (getCountRef() && getCountRef()->empty())
    return emitOpError("the count name must not be empty");
  return success();
}

// sea.print.mem "tag" = "mem"["addr"] : type attr-dict
// sea.print.mem "tag" %m[%p] (, %n)? : memtype (, ntype)? -> type attr-dict
ParseResult PrintMemOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr tag;
  if (parser.parseAttribute(tag, parser.getBuilder().getNoneType(), "tag",
                            result.attributes))
    return failure();
  Type t;
  if (succeeded(parser.parseOptionalEqual())) {
    StringAttr mem, addr;
    if (parser.parseAttribute(mem, parser.getBuilder().getNoneType(), "mem",
                              result.attributes) ||
        parser.parseLSquare() ||
        parser.parseAttribute(addr, parser.getBuilder().getNoneType(), "addr",
                              result.attributes) ||
        parser.parseRSquare() || parser.parseColonType(t))
      return failure();
  } else {
    OpAsmParser::UnresolvedOperand m, a, n;
    bool hasCount = false;
    Type mt, nt;
    if (parser.parseOperand(m) || parser.parseLSquare() ||
        parser.parseOperand(a) || parser.parseRSquare())
      return failure();
    if (succeeded(parser.parseOptionalComma())) {
      hasCount = true;
      if (parser.parseOperand(n))
        return failure();
    }
    if (parser.parseColonType(mt))
      return failure();
    if (hasCount && (parser.parseComma() || parser.parseType(nt)))
      return failure();
    if (parser.parseArrow() || parser.parseType(t))
      return failure();
    auto memTy = dyn_cast<MemType>(mt);
    if (!memTy)
      return parser.emitError(parser.getNameLoc(), "expected a memory type");
    if (parser.resolveOperand(m, mt, result.operands) ||
        parser.resolveOperand(a, memTy.getIndex(), result.operands) ||
        (hasCount && parser.resolveOperand(n, nt, result.operands)))
      return failure();
  }
  result.addAttribute("type", TypeAttr::get(t));
  return parser.parseOptionalAttrDict(result.attributes);
}

void PrintMemOp::print(OpAsmPrinter &p) {
  p << " ";
  p.printAttributeWithoutType(getTagAttr());
  SmallVector<StringRef> elided{"tag", "type"};
  if (!isStrong()) {
    p << " = ";
    p.printAttributeWithoutType(getMemAttr());
    p << "[";
    p.printAttributeWithoutType(getAddrAttr());
    p << "] : " << getType();
    elided.append({"mem", "addr"});
  } else {
    p << " " << getMemValue() << "[" << getAddrValue() << "]";
    if (Value n = getCountValue())
      p << ", " << n;
    p << " : " << getMemValue().getType();
    if (Value n = getCountValue())
      p << ", " << n.getType();
    p << " -> " << getType();
  }
  p.printOptionalAttrDict((*this)->getAttrs(), elided);
}

LogicalResult PrintScopeOp::verify() {
  if (getTag().empty())
    return emitOpError("the tag must not be empty");
  return success();
}

LogicalResult PrintEndOp::verify() {
  if (getTag().empty())
    return emitOpError("the tag must not be empty");
  return success();
}

} // namespace sea
