// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- OpSem.cpp - symbolic execution of sea programs -----------*- C++ -*-===//

#include "sea/VC/OpSem.h"
#include "sea/Expr/ExprOpBinder.hh"
#include "sea/Expr/ExprOverflow.hh"
#include "sea/Expr/SeaMRules.hh"
#include "sea/Transforms/WidthDomain.h"

#include "sea/Expr/ExprAPInt.hh"
#include "sea/Expr/ExprMlir.hh"

#include "sea/Support/Diag.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace expr;
using namespace expr::op;

namespace sea {

OpSemContext::OpSemContext(SymStore &values, ExprVector &side)
    : store(values), sideConds(side),
      pathCond(mk<TRUE>(values.getExprFactory())) {}

void OpSemContext::addScopedSide(Expr e) {
  sideConds.push_back(boolop::limp(pathCond, e));
}

void OpSemContext::addScopedGuarantee(Expr e) {
  guaranteeConds.push_back(boolop::limp(pathCond, e));
}

SeaOpSem::SeaOpSem(ExprFactory &efac, ValueNames &names, bool nativeMulOverflow,
                   bool seam, const SeaMOptions *seamOpts)
    : ef(efac), names(names), nativeMulOverflow(nativeMulOverflow),
      seamMode(seam), trueE(mk<TRUE>(efac)), falseE(mk<FALSE>(efac)),
      bv1One(bv::bvnum(mpz_class(1UL), 1, efac)),
      bv1Zero(bv::bvnum(mpz_class(0UL), 1, efac)) {
  // one rewriter and cache per VC: SeaM's memory rules (seam), the
  // normalization of every VC term (simplify), or both
  if (seam || (seamOpts && seamOpts->simplify)) {
    SeaMOptions o = seamOpts ? *seamOpts : SeaMOptions();
    if (!seam) {
      // plain bit-vector rules only: no memory rewriting, no assumption
      o.memoryRules = false;
      o.assumeInBounds = false;
      o.arm = false;
      o.storeMap = false;
    }
    RewriteOptions ro;
    ro.maxRuleCalls = o.maxRuleCalls;
    seamRw = std::make_unique<SeaMRewriter>(efac, bases, o, ro);
    simplifying = o.simplify;
    wideMode = seam && o.wideAccess;
  }
}

SeaOpSem::~SeaOpSem() = default;

const RewriteStats *SeaOpSem::seamStats() const {
  return seamRw ? &seamRw->stats() : nullptr;
}

double SeaOpSem::seamRewriteSeconds() const {
  return seamRw ? seamRw->rewriteSeconds() : 0;
}

double SeaOpSem::simplifySeconds() const {
  return seamRw ? seamRw->simplifySeconds() : 0;
}

Expr SeaOpSem::simplify(Expr e) {
  return simplifying ? seamRw->simplify(e) : e;
}

/// The solver boundary (SeaM.md "Meaning of SeaM terms"; StoreMap.hh,
/// MemOps.hh): memory values of the VC may hold store-maps and memory-operation
/// terms, neither an SMT term. Whatever goes to a solver or to a model
/// evaluation passes here: a semantics-only translation (store-maps into
/// stores, each term eliminated by its read equation), no assumption.
Expr SeaOpSem::solverTerm(Expr e, std::string &why) {
  if (seamRw)
    return seamRw->solverTerm(e, why);
  // no rewriter (S2g): only the pointer operators and sort to erase, with
  // the lowering's arithmetic (ExprOpPtr.hh)
  if (!ptrSorted())
    return e;
  if (!plainEraser)
    plainEraser = std::make_unique<PtrIndexExpander>();
  return plainEraser->expand(e);
}

bool SeaOpSem::hasPtrTerms(const Expr &e) {
  if (seamRw)
    return seamRw->hasPtrIndex(e);
  if (!plainEraser)
    plainEraser = std::make_unique<PtrIndexExpander>();
  return plainEraser->has(e);
}

Expr SeaOpSem::solverTerm(Expr e) {
  std::string why;
  Expr res = solverTerm(e, why);
  if (!res)
    diag::error("vc.memop-term-at-boundary",
                "the solver boundary cannot translate the term: " + why +
                    " (memory-operation terms are translated by their read "
                    "equations only; docs/design/SeaM-memops.md)");
  return res;
}

Expr SeaOpSem::firingObligation(const RewriteTrail::Firing &f,
                                std::string &why) {
  Expr l = f.lhs, r = f.rhs;
  // A memory-valued firing over a memory-operation term: the equality of the
  // two memories, by extensionality, as the equality of their cells at a
  // fresh index (equivalent: some cell differs iff the arrays differ), so
  // that the boundary sees reads only.
  if (seamRw && (seamRw->memOpsMade() || seamRw->wideMade()) &&
      isMemoryHead(l)) {
    TypeChecker tc;
    Expr sort = tc.typeOf(l);
    if (!isOpX<ARRAY_TY>(sort)) {
      why = "a memory-valued firing whose sort is not an array";
      return Expr();
    }
    Expr cell = bind::mkConst(mkTerm<std::string>("seam-check.cell", ef),
                              sort::arrayIndexTy(sort));
    l = op::array::select(l, cell);
    r = op::array::select(r, cell);
  }
  Expr ls = solverTerm(l, why);
  if (!ls)
    return Expr();
  Expr rs = solverTerm(r, why);
  if (!rs)
    return Expr();
  return mk<NEG>(mk<EQ>(ls, rs));
}

void SeaOpSem::setTrail(RewriteTrail *t) {
  if (seamRw)
    seamRw->stats().trail = t;
}

void SeaOpSem::setTrailContext(llvm::StringRef kind) {
  RewriteTrail *t = seamRw ? seamRw->stats().trail : nullptr;
  if (t)
    t->setContext(kind, nullptr, 0, [] { return std::string(); });
}

/// file:line:col of a location (the first file location in it), else its
/// printed form.
static std::string locText(Location loc) {
  if (auto fl = loc->findInstanceOf<FileLineColLoc>())
    return (fl.getFilename().strref() + ":" + Twine(fl.getLine()) + ":" +
            Twine(fl.getColumn()))
        .str();
  std::string s;
  llvm::raw_string_ostream os(s);
  loc.print(os);
  return s;
}

void SeaOpSem::noteInbViolation(Location loc, llvm::StringRef kind,
                                llvm::function_ref<std::string()> reason) {
  // the reason (a walk of the address) only for the notes printed
  if (++inbStaticViolations <= kInbViolationNotes)
    inbNotes.push_back(locText(loc) + " " + kind.str() + ": " + reason());
}

void SeaOpSem::trailAccess(const char *kind, uint64_t units) {
  RewriteTrail *t = seamRw ? seamRw->stats().trail : nullptr;
  if (!t)
    return;
  Operation *op = curOp;
  t->setContext(kind, op, units,
                [op] { return op ? locText(op->getLoc()) : std::string(); });
}

Expr SeaOpSem::firingPremise(const RewriteTrail &trail,
                             const RewriteTrail::Firing &f, std::string &why) {
  // composed (SeaM-mixed-width.md rev 5): the A-INB / LAYOUT part and the
  // ALIGN part (the accesses' `align` annotations: trusted front-end facts)
  Expr base;
  if (f.premises & seam::PremAInb) {
    base = accessPremise(trail, f, why);
    if (!base)
      return Expr();
  } else if (f.premises & seam::PremLayout) {
    // LAYOUT alone (shadow.distinct-base): the placement equations, a fact
    // of the VC (its side conditions), nothing about the program
    ExprVector conj;
    for (const ObjectBase &o : bases.objects())
      conj.push_back(mk<EQ>(mk<ADDR>(o.constant), bvnum(o.base)));
    base = mknary<AND>(trueE, conj);
  } else {
    base = trueE;
  }
  if (f.premises & seam::PremPtrIndex) {
    // the promises of the read's address nodes (PtrIndex.md section 5),
    // guarded by the address's ite conditions (the selected leaf's promise),
    // over expanded terms
    Expr prom = seamRw->rules().accessPtrIndexPremise(f.lhs);
    if (!prom) {
      why = "a PTR_INDEX firing with no promised node at its read";
      return Expr();
    }
    base = mk<AND>(base, seamRw->expandPtrIndex(prom));
  }
  if (!(f.premises & seam::PremAlign))
    return base;
  llvm::SmallVector<std::pair<Expr, uint64_t>, 2> aligns;
  seamRw->rules().accessAligns(f.lhs, aligns);
  ExprVector conj{base};
  for (const auto &[a, al] : aligns) {
    unsigned w = seamRw->rules().widthOf(a);
    conj.push_back(seamRw->expandPtrIndex(
        mk<EQ>(mk<BAND>(mk<ADDR>(a), bv::bvnum(mpz_class(al - 1), w, ef)),
               bv::bvnum(mpz_class(0UL), w, ef))));
  }
  return mknary<AND>(trueE, conj);
}

Expr SeaOpSem::layoutFacts() {
  ExprVector conj;
  for (const ObjectBase &o : bases.objects())
    conj.push_back(mk<EQ>(mk<ADDR>(o.constant), bvnum(o.base)));
  // for a solver: erased (the object constants are ptr(w), registered with
  // SeaM only)
  return solverTerm(mknary<AND>(trueE, conj));
}

Expr SeaOpSem::accessPremise(const RewriteTrail &trail,
                             const RewriteTrail::Firing &f, std::string &why) {
  // a firing at a memory-operation term itself (addr.prune-size0 over its
  // ranges, SeaM-provenance.md): the premise is the A-INB of the
  // term's original ranges (the lhs, before the rewrite), whatever access
  // the rewriting happened under; the obligation is the equality of the two
  // memories (firingObligation)
  if (isMemOpTerm(f.lhs)) {
    ExprVector conj;
    for (const ObjectBase &o : bases.objects())
      conj.push_back(mk<EQ>(mk<ADDR>(o.constant), bvnum(o.base)));
    llvm::SmallVector<std::pair<Expr, Expr>, 2> ranges;
    seamRw->rules().accessRanges(f.lhs, ranges);
    for (const auto &[d, n] : ranges) {
      bool violated = false;
      Expr inb = seamRw->rules().rangeInBounds(d, n, violated);
      if (violated) {
        why = "a range the firing relies on is statically out of bounds";
        return Expr();
      }
      conj.push_back(inb);
    }
    return mknary<AND>(trueE, conj);
  }
  const RewriteTrail::Context &c = trail.context(f.ctx);
  if (!c.units) {
    why = "A-INB firing outside a memory access (context " + c.kind + ")";
    return Expr();
  }
  // per-access spans (docs/design/SeaM-mixed-width.md): each address with
  // the width of its own access
  llvm::SmallVector<std::pair<Expr, uint64_t>, 4> spans;
  if (!seamRw->rules().accessSpans(f.lhs, c.units, spans)) {
    why = "A-INB firing on a term that is not a select, store or wselect, "
          "or an overlap of recorded access bytes";
    return Expr();
  }
  ExprVector conj;
  // the layout: a fact of the VC (its side conditions), needed to read
  // "different objects" as "different addresses"
  for (const ObjectBase &o : bases.objects())
    conj.push_back(mk<EQ>(mk<ADDR>(o.constant), bvnum(o.base)));
  for (const auto &[a, units] : spans) {
    bool violated = false;
    Expr inb = seamRw->rules().inBounds(a, units, violated);
    if (violated) {
      why = "an access the firing relies on is statically out of bounds";
      return Expr();
    }
    conj.push_back(inb);
  }
  // the destination range of a memory-operation term below a read
  // (MemOps.hh): every cell of [d, d + n) in d's object
  llvm::SmallVector<std::pair<Expr, Expr>, 2> ranges;
  seamRw->rules().accessRanges(f.lhs, ranges);
  for (const auto &[d, n] : ranges) {
    bool violated = false;
    Expr inb = seamRw->rules().rangeInBounds(d, n, violated);
    if (violated) {
      why = "a range the firing relies on is statically out of bounds";
      return Expr();
    }
    conj.push_back(inb);
  }
  return mknary<AND>(trueE, conj);
}

/// A-INB (SeaMRules.hh) is an assumption about the program: every access
/// address that normalizes to a pointer into object k lies in object k. It is
/// assumed and reported, not discharged (docs/design/SeaM.md, "A-INB: assumed
/// and reported"). Each access is counted here; a leaf with a numeral offset
/// that is out of bounds (or an object smaller than the access) is a definite
/// violation, which sea-vc answers by running without the assumption (a
/// sanity check, not a soundness claim).
Expr SeaOpSem::accessAddr(Expr mem, Expr ptr, uint64_t units,
                          OpSemContext &ctx) {
  if (!seamMode)
    return ptr;
  // the address may hold loads of other memories (other access sizes): no
  // access size for its firings (an A-INB firing there is unexpressible)
  trailAccess("addr", 0);
  ptr = seamRw->rewrite(ptr);
  // a shadow access (keyed by object identity, SeaM-null.md "Shadow
  // memories") is not a program access: no A-INB condition, no violation
  if (seamRw->rules().shadowMemory(mem)) {
    ++seamRw->stats().rules["shadow.access"];
    return ptr;
  }
  if (keyedAccess()) {
    ++seamRw->stats().rules["companion.access"];
    pendingKeyed.push_back({ptr, curOp->getLoc()});
    return ptr;
  }
  blockAddrs.insert(ptr);
  seamRw->rules().noteInbAccess(ptr, units);
  ++inbAccesses;
  bool violated = false;
  Expr inb = seamRw->rules().inBounds(ptr, units, violated);
  if (violated)
    noteInbViolation(curOp->getLoc(), curOp->getName().stripDialect(), [&] {
      return seamRw->rules().explainViolation(ptr, units, false);
    });
  if (!isOpX<TRUE>(inb))
    inbSeen.insert(mk<AND>(ctx.getPathCond(), boolop::lneg(inb)));
  return ptr;
}

Expr SeaOpSem::mkLambda(Expr idx, const std::function<Expr(Expr)> &body) {
  Expr b0 = bind::bvar(0, idx);
  Expr decl = bind::fname(bind::mkConst(mkTerm<std::string>("i", ef), idx));
  return mk<LAMBDA>(decl, body(b0));
}

bool SeaOpSem::isMemoryReg(Expr reg) { return bind::isArrayConst(reg); }

bool SeaOpSem::hasFreeBoundVar(Expr e) {
  // (node, binder depth) pairs seen; a bound variable with index >= depth
  // is free
  std::set<std::pair<ENode *, unsigned>> seen;
  std::vector<std::pair<Expr, unsigned>> stack{{e, 0}};
  while (!stack.empty()) {
    auto [n, d] = stack.back();
    stack.pop_back();
    if (!seen.insert({n.get(), d}).second)
      continue;
    if (bind::isBVar(n)) {
      if (bind::bvarId(n) >= d)
        return true;
      continue;
    }
    if (isOpX<LAMBDA>(n)) {
      stack.push_back({bind::body(n), d + bind::numBound(n)});
      continue;
    }
    for (unsigned i = 0; i < n->arity(); ++i)
      stack.push_back({n->arg(i), d});
  }
  return false;
}

Expr SeaOpSem::indexSortOf(Expr arrayConst) {
  return sort::arrayIndexTy(bind::rangeTy(bind::fname(arrayConst)));
}

Expr SeaOpSem::coerceMemory(Expr v, Expr reg) {
  if (isOpX<LAMBDA>(v))
    return v;
  if (isOpX<ITE>(v)) {
    // a join of memories: one lambda, as SeaHorn's bind::lite /
    // push_ite_lambda (ExprOpBinder.hh:265-319):
    //   ite(c, L1, L2)  ~>  lambda i. ite(c, fapp(L1, i), fapp(L2, i))
    // (the applications are beta-reduced by the simplifier); z3 does not
    // push a select through an ite of arrays (expand_select_ite is off), so
    // a read of an ite of lambdas would stay unreduced
    Expr t = coerceMemory(v->arg(1), reg), e = coerceMemory(v->arg(2), reg);
    if (!t || !e)
      return Expr();
    Expr c = v->arg(0);
    return mkLambda(indexSortOf(reg), [&](Expr i) {
      return mk<ITE>(c, bind::fapp(t, i), bind::fapp(e, i));
    });
  }
  // an array term (a constant, a store, a const array: what z3's simplifier
  // may return for a lambda): lambda i. select(v, i)
  return mkLambda(indexSortOf(reg),
                  [&](Expr i) { return op::array::select(v, i); });
}

/// The index sort of a memory: a memory indexed by addresses is
/// array(ptr(w), bv(c)) when addresses are pointers (ptrSorted; PtrSort
/// S2e / S2g, D1: the Select / Store typing rule finds an integer at an
/// access position); else bv(w).
static Expr memIndexSort(MemType memTy, bool ptrSorted, ExprFactory &ef) {
  if (ptrSorted && isa<AddrType>(memTy.getIndex()))
    return op::ptr::ptrsort(memTy.getIndexWidth(), ef);
  return bv::bvsort(memTy.getIndexWidth(), ef);
}

/// The index sort of the memory operand of the current op (lambda encoding).
static Expr curIndexSort(Operation *op, ExprFactory &ef) {
  auto memTy = cast<MemType>(op->getOperand(0).getType());
  return bv::bvsort(memTy.getIndexWidth(), ef);
}

Expr SeaOpSem::memLoad(Expr mem, Expr ptr, uint64_t units, OpSemContext &ctx) {
  if (lambdaMode)
    return bind::fapp(mem, ptr);
  ptr = accessAddr(mem, ptr, units, ctx);
  trailAccess("load", units);
  Expr sel = op::array::select(mem, ptr);
  return seamMode ? seamRw->rewrite(sel) : sel;
}

Expr SeaOpSem::memStore(Expr mem, Expr ptr, Expr val, uint64_t units,
                        OpSemContext &ctx) {
  if (lambdaMode)
    return mkLambda(curIndexSort(curOp, ef), [&](Expr i) {
      return mk<ITE>(mk<EQ>(i, ptr), val, bind::fapp(mem, i));
    });
  ptr = accessAddr(mem, ptr, units, ctx);
  trailAccess("store", units);
  Expr st = op::array::store(mem, ptr, val);
  return seamMode ? seamRw->rewrite(st) : st;
}

static unsigned widthOf(Value v) { return bitWidthOf(v.getType()); }

/// The `align` annotation of the current load / store (0: none).
uint64_t SeaOpSem::curAlign() const {
  if (auto ld = dyn_cast_if_present<LoadOp>(curOp))
    return ld.getAlign().value_or(0);
  if (auto st = dyn_cast_if_present<StoreOp>(curOp))
    return st.getAlign().value_or(0);
  return 0;
}

// Width terms (docs/design/SeaM-mixed-width.md): A-INB is stated on the whole
// span (bits / 8 bytes), as the trail context's access size, so a firing's
// premise is the span of this access (per-access width accounting).
Expr SeaOpSem::memLoadWide(Expr mem, Expr ptr, unsigned bits,
                           OpSemContext &ctx) {
  uint64_t bytes = bits / 8;
  ptr = accessAddr(mem, ptr, bytes, ctx);
  trailAccess("load", bytes);
  seamRw->noteWide();
  return seamRw->rewrite(
      op::array::wselect(mem, ptr, bv::bvsort(bits, ef), curAlign()));
}

Expr SeaOpSem::memStoreWide(Expr mem, Expr ptr, Expr val, unsigned bits,
                            OpSemContext &ctx) {
  uint64_t bytes = bits / 8;
  ptr = accessAddr(mem, ptr, bytes, ctx);
  trailAccess("store", bytes);
  seamRw->noteWide();
  return seamRw->rewrite(op::array::wstore(mem, ptr, val, curAlign()));
}

/// The address units one access of `op` (a load, store or range op over its
/// first operand's memory) covers: a cell.
static uint64_t unitsOf(Operation &op) {
  return cast<MemType>(op.getOperand(0).getType()).getCellUnits();
}

Expr SeaOpSem::mkSymbReg(Value v) {
  // One Expr per value for the whole VC: building it (the name
  // terminal, the sort, the constant) is a few hash-consed allocations that
  // every read, write and havoc of the value would repeat.
  auto it = symbRegs.find(v);
  if (it != symbRegs.end())
    return it->second;
  Expr name = mkTerm(ValueRef{v, names.name(v)}, ef);
  Expr reg;
  // Array encoding: a memory is an SMT array from addresses to cells.
  if (auto mem = dyn_cast<MemType>(v.getType()))
    reg = bind::mkConst(
        name, sort::arrayTy(memIndexSort(mem, ptrSorted(), ef),
                            bv::bvsort(mem.getCell().getWidth(), ef)));
  // an address is a pointer (PtrSort S2e / S2g): ptr(w)
  else if (ptrSorted() && isa<AddrType>(v.getType()))
    reg = op::ptr::ptrConst(name, widthOf(v));
  else
    reg = bv::bvConst(name, widthOf(v));
  return symbRegs[v] = reg;
}

Expr SeaOpSem::numeral(const llvm::APInt &v) { return bvnum(v); }

Expr SeaOpSem::mkSymbReg(Block *b) {
  auto it = blockRegs.find(b);
  if (it != blockRegs.end())
    return it->second;
  return blockRegs[b] = bind::boolConst(mkTerm(BlockRef{b, names.name(b)}, ef));
}

Expr SeaOpSem::toBool(Expr bv1) { return mk<EQ>(bv1, bv1One); }

Expr SeaOpSem::fromBool(Expr b) { return mk<ITE>(b, bv1One, bv1Zero); }

Expr SeaOpSem::bvnum(const llvm::APInt &v) {
  // toMpz reads its argument as signed; widen by a bit to keep it unsigned.
  return bv::bvnum(toMpz(v.zext(v.getBitWidth() + 1)), v.getBitWidth(), ef);
}

static Expr mkICmp(ICmpPredicate p, Expr a, Expr b) {
  switch (p) {
  case ICmpPredicate::eq:
    return mk<EQ>(a, b);
  case ICmpPredicate::ne:
    return mk<NEQ>(a, b);
  case ICmpPredicate::slt:
    return mk<BSLT>(a, b);
  case ICmpPredicate::sle:
    return mk<BSLE>(a, b);
  case ICmpPredicate::sgt:
    return mk<BSGT>(a, b);
  case ICmpPredicate::sge:
    return mk<BSGE>(a, b);
  case ICmpPredicate::ult:
    return mk<BULT>(a, b);
  case ICmpPredicate::ule:
    return mk<BULE>(a, b);
  case ICmpPredicate::ugt:
    return mk<BUGT>(a, b);
  case ICmpPredicate::uge:
    return mk<BUGE>(a, b);
  }
  llvm_unreachable("unknown icmp predicate");
}

void SeaOpSem::addShadowRoot(Expr start) {
  if (bases.isShadowRoot(start))
    return;
  bases.addShadowRoot(start);
  seamRw->rules().forgetShadowMemo();
}

LogicalResult SeaOpSem::exec(Block &b, OpSemContext &ctx) {
  // the pointer companion (S6) only where a split pointer load reads it
  if (ptrCompanion() && !companionNeeded) {
    bool any = false;
    b.getParentOp()->walk([&](AddrFromBitsOp fb) {
      any |= fb.getOperand().getDefiningOp<ConcatOp>() != nullptr;
    });
    companionNeeded = any;
  }
  // The shadow memories of the program first (their starts with a constant
  // value), so that no term is classified before its start is known
  // (Rules::shadowMemory is memoized; a data memory with the same start
  // term is then read as shadow from its first access).
  if (seamMode && !shadowScanned) {
    shadowScanned = true;
    b.getParentOp()->walk([&](MemConstOp mc) {
      auto c = mc.getValue().getDefiningOp<ConstOp>();
      if (!mc.getShadow() || !c)
        return;
      addShadowRoot(op::array::constArray(
          memIndexSort(mc.getType(), ptrSorted(), ef),
          bvnum(cast<IntegerAttr>(c.getValue()).getValue())));
    });
  }
  // companion (keyed) accesses of this block wait for their mirrored
  // program access (see keyedAccess)
  blockAddrs.clear();
  blockRanges.clear();
  pendingKeyed.clear();
  pendingKeyedRanges.clear();
  // the defHook of the results of the op before (the loop below leaves an
  // op at many places)
  Operation *defined = nullptr;
  auto runDefHook = [&]() {
    if (defined && ctx.defHook)
      for (Value r : defined->getResults())
        ctx.defHook(r);
    defined = nullptr;
  };
  for (Operation &op : b.without_terminator()) {
    runDefHook();
    // prints (weak or strong) encode nothing: no term, no side constraint
    // (docs/design/Print.md); their operands cost what their defs cost
    if (isPrintOp(&op))
      continue;
    defined = &op;
    curOp = &op;
    auto in = [&](unsigned i) { return lookup(op.getOperand(i), ctx); };
    // A segment is the base address of a placed object. By default it is
    // the numeral. With SeaM it is the named constant obj.<name>, equal to
    // that numeral by a side constraint (a fact about the layout, true on
    // every path), and the object goes to the base registry.
    if (auto seg = dyn_cast<SegmentOp>(op)) {
      Expr base = bvnum(seg.getBase());
      if (seamMode) {
        Expr obj = op::ptr::ptrConst(
            mkTerm<std::string>("obj." + seg.getName().str(), ef),
            seg.getBase().getBitWidth());
        // The rewriter takes the constants for distinct objects with
        // disjoint address ranges (placement makes them so; a hand-written
        // placed program may not).
        if (bases.isBase(obj)) {
          diag::error("vc.seam-segment-duplicate",
                      "two sea.segment ops are named \"" + seg.getName().str() +
                          "\": --seam needs one segment per object",
                      op.getLoc());
          return failure();
        }
        if (const ObjectBase *o =
                bases.overlapping(seg.getBase(), seg.getSize())) {
          diag::error("vc.seam-segment-overlap",
                      "sea.segment \"" + seg.getName().str() +
                          "\" overlaps \"" + o->name +
                          "\": --seam needs disjoint objects (pointers into "
                          "different objects are taken to differ)",
                      op.getLoc());
          return failure();
        }
        bases.add(obj, seg.getName().str(), seg.getKind().str(), seg.getBase(),
                  seg.getSize(), seg.getAlign());
        // its erased twin (the same name at bv(w)) names it too
        bases.addAlias(
            bv::bvConst(mkTerm<std::string>("obj." + seg.getName().str(), ef),
                        seg.getBase().getBitWidth()),
            obj);
        // the layout: the address of the object (ADDR, S2d)
        ctx.addSide(mk<EQ>(mk<ADDR>(obj), base));
        base = obj;
      } else if (ptrSorted()) {
        // no SeaM: the pointer whose address is the numeral (S2g)
        base = mk<PTR_FROM_INT>(base);
      }
      ctx.write(mkSymbReg(seg.getResult()), base);
      continue;
    }
    // Ops whose effect is a fact, not a value.
    if (auto assume = dyn_cast<AssumeOp>(op)) {
      assumeSides.push_back(ctx.side().size()); // EXPERIMENT: --x-read-table
      ctx.addScopedSide(toBool(in(0)));
      continue;
    }
    if (auto check = dyn_cast<AssertOp>(op)) {
      // the A-PTRINT discharge checks the premise only
      if (!(ptrIntChecking() && companionNeeded.value_or(false)))
        ctx.addScopedGuarantee(toBool(in(0)));
      continue;
    }
    if (auto nd = dyn_cast<NondetOp>(op)) {
      ctx.havoc(mkSymbReg(nd.getResult()));
      continue;
    }
    if (auto mn = dyn_cast<MemNondetOp>(op)) {
      Expr reg = mkSymbReg(mn.getResult());
      Expr a = ctx.havoc(reg);
      // lambda encoding: the havoced array, coerced to a function (the
      // array is kept for replay: initialArray)
      if (lambdaMode) {
        havocs[&op] = a;
        ctx.write(reg, coerceMemory(a, reg));
      }
      continue;
    }
    if (auto mc = dyn_cast<MemConstOp>(op)) {
      auto memTy = mc.getType();
      Expr start = op::array::constArray(memIndexSort(memTy, ptrSorted(), ef),
                                         lookup(mc.getValue(), ctx));
      // A shadow memory (sea-lower-tracking; SeaM-null.md, "Shadow
      // memories"): its start goes to the registry, and every memory term
      // over it is read by object identity, without A-INB. A data memory
      // with the same start term (same sort, same value) is then read the
      // same way: still sound (no assumption is added), less precise;
      // reported.
      if (seamMode) {
        if (mc.getShadow())
          addShadowRoot(start);
        else
          dataConstRoots.insert(start);
        if (bases.isShadowRoot(start) && dataConstRoots.count(start))
          diag::warn("vc.seam-shadow-shared-start",
                     "a data memory starts with the same term as a shadow "
                     "memory (sea.mem.const of one sort and value): its "
                     "accesses are read as shadow accesses, without A-INB "
                     "(sound, less precise)",
                     op.getLoc());
      }
      if (lambdaMode)
        start = mkLambda(memIndexSort(memTy, ptrSorted(), ef),
                         [&](Expr) { return lookup(mc.getValue(), ctx); });
      ctx.write(mkSymbReg(mc.getResult()), start);
      continue;
    }
    // Wide accesses are split by a pass (sea-split-wide-access), not here;
    // with width terms (SeaMOptions::wideAccess) a wide access of a byte
    // memory is one wselect / wstore (docs/design/SeaM-mixed-width.md).
    if (isa<LoadOp, StoreOp>(op)) {
      Value val = isa<LoadOp>(op) ? op.getResult(0) : op.getOperand(2);
      auto memTy = cast<MemType>(op.getOperand(0).getType());
      unsigned bits = widthOf(val);
      if (wideMode && inWideByteDomain(memTy, bits)) {
        auto in = [&](unsigned i) { return lookup(op.getOperand(i), ctx); };
        curOp = &op;
        // an address value is stored as its bits and loaded as a pointer
        // (PTR_TO_BITS / PTR_FROM_BITS, S1.6b; seamMode: wideMode is SeaM's)
        const bool addrVal = isa<AddrType>(val.getType());
        Expr res = isa<LoadOp>(op)
                       ? memLoadWide(in(0), in(1), bits, ctx)
                       : memStoreWide(in(0), in(1),
                                      addrVal ? mk<PTR_TO_BITS>(in(2)) : in(2),
                                      bits, ctx);
        if (addrVal && isa<LoadOp>(op))
          res = mk<PTR_FROM_BITS>(res);
        ctx.write(mkSymbReg(op.getResult(0)), res);
        continue;
      }
      if (bits != memTy.getCell().getWidth()) {
        diag::error("vc.wide-access",
                    op.getName().getStringRef() +
                        " spans several cells: the VC handles single-cell "
                        "accesses (run sea-split-wide-access, or sea-vc "
                        "--lower)",
                    op.getLoc());
        return failure();
      }
    }
    // The pointer mode at the encoding site (S2f): opaque pointers have no
    // int-to-ptr.
    if (ptrSorted() && !addressMode && isa<AddrFromIntOp>(op)) {
      diag::error("vc.int-to-ptr",
                  "addr.from_int in opaque pointer mode (an int-to-ptr the "
                  "mode classification missed: a bug; use --ptr-mode=address)",
                  op.getLoc());
      return failure();
    }
    // Structs are scalarized by a pass (sea-scalarize-structs).
    if (isa<StructCreateOp, StructExtractOp, StructInsertOp>(op) ||
        llvm::any_of(op.getResultTypes(),
                     [](Type t) { return isa<StructType>(t); })) {
      diag::error("vc.struct",
                  op.getName().getStringRef() +
                      " works on structs: the VC encodes their fields (run "
                      "sea-scalarize-structs, or sea-vc --lower)",
                  op.getLoc());
      return failure();
    }
    // Intrinsics are expanded by a pass (sea-expand-intrinsics), not here;
    // mul overflow only if the solver lacks it.
    bool nativeMul = nativeMulOverflow && isa<UMulOOp, SMulOOp>(op);
    if (!nativeMul && isa<UMinOp, UMaxOp, SMinOp, SMaxOp, BSwapOp, CtPopOp,
                          FShlOp, FShrOp, UAddOOp, SAddOOp, USubOOp, SSubOOp,
                          UMulOOp, SMulOOp, SDivOOp, NegOOp>(op)) {
      diag::error("vc.intrinsic",
                  op.getName().getStringRef() +
                      " is an intrinsic the VC does not encode for this "
                      "solver (run sea-expand-intrinsics, or sea-vc --lower)",
                  op.getLoc());
      return failure();
    }
    if (isa<MemSetOp, MemMoveOp, MemHavocOp>(op)) {
      if (failed(execRangeOp(op, ctx)))
        return failure();
      continue;
    }

    Expr val =
        llvm::TypeSwitch<Operation *, Expr>(&op)
            .Case([&](ConstOp c) {
              return bvnum(cast<IntegerAttr>(c.getValue()).getValue());
            })
            .Case([&](AddOp) { return mk<BADD>(in(0), in(1)); })
            .Case([&](SubOp) { return mk<BSUB>(in(0), in(1)); })
            .Case([&](MulOp) { return mk<BMUL>(in(0), in(1)); })
            .Case([&](UDivOp) { return mk<BUDIV>(in(0), in(1)); })
            .Case([&](SDivOp) { return mk<BSDIV>(in(0), in(1)); })
            .Case([&](URemOp) { return mk<BUREM>(in(0), in(1)); })
            .Case([&](SRemOp) { return mk<BSREM>(in(0), in(1)); })
            .Case([&](AndOp) { return mk<BAND>(in(0), in(1)); })
            .Case([&](OrOp) { return mk<BOR>(in(0), in(1)); })
            .Case([&](XorOp) { return mk<BXOR>(in(0), in(1)); })
            .Case([&](ShlOp) { return mk<BSHL>(in(0), in(1)); })
            .Case([&](LShrOp) { return mk<BLSHR>(in(0), in(1)); })
            .Case([&](AShrOp) { return mk<BASHR>(in(0), in(1)); })
            .Case([&](ICmpOp c) {
              return fromBool(mkICmp(c.getPredicate(), in(0), in(1)));
            })
            .Case(
                [&](SelectOp) { return mk<ITE>(toBool(in(0)), in(1), in(2)); })
            .Case([&](ZExtOp z) {
              return bv::zext(in(0), widthOf(z.getResult()));
            })
            .Case([&](SExtOp s) {
              return bv::sext(in(0), widthOf(s.getResult()));
            })
            .Case([&](TruncOp t) {
              return bv::extract(widthOf(t.getResult()) - 1, 0, in(0));
            })
            .Case([&](ConcatOp) { return bv::concat(in(0), in(1)); })
            // Native mul overflow (z3's Z3_mk_bvmul_no_overflow /
            // _no_underflow; SMT-LIB 2.7 bvumulo / bvsmulo).
            .Case([&](UMulOOp) {
              return fromBool(bv::mulOverflows(false, in(0), in(1)));
            })
            .Case([&](SMulOOp) {
              return fromBool(bv::mulOverflows(true, in(0), in(1)));
            })
            .Case([&](ExtractOp e) {
              return bv::extract(e.getLow() + widthOf(e.getResult()) - 1,
                                 e.getLow(), in(0));
            })
            .Case([&](PtrIndexOp p) { return ptrIndexTerm(p, ctx); })
            // Addresses are bit-vectors in the VC.
            .Case([&](AddrAddOp) { return addrAdd(in(0), in(1)); })
            .Case([&](AddrDiffOp) {
              // a PTR_DIFF (SeaM: its diff.* rules), else the bvsub
              return ptrSorted() ? mk<PTR_DIFF>(in(0), in(1))
                                 : mk<BSUB>(in(0), in(1));
            })
            .Case([&](AddrCmpOp c) {
              // an ordered compare observes the addresses (ADDR, S1.6);
              // eq / ne compare pointers (PTR_EQ, PtrSort.md D6)
              ICmpPredicate pr = c.getPredicate();
              bool ordered = pr != ICmpPredicate::eq && pr != ICmpPredicate::ne;
              if (ptrSorted() && ordered)
                return fromBool(mkICmp(pr, mk<ADDR>(in(0)), mk<ADDR>(in(1))));
              // eq / ne: pointer equality (PTR_EQ, S2d), erased to EQ
              if (ptrSorted())
                return fromBool(pr == ICmpPredicate::eq
                                    ? mk<PTR_EQ>(in(0), in(1))
                                    : mk<NEG>(mk<PTR_EQ>(in(0), in(1))));
              return fromBool(mkICmp(pr, in(0), in(1)));
            })
            .Case([&](AddrToIntOp) {
              // an escape (PTR_TO_INT, S1.6c)
              return ptrSorted() ? mk<PTR_TO_INT>(in(0)) : in(0);
            })
            .Case([&](AddrFromIntOp) {
              // provenance lost (PTR_FROM_INT, S1.6d; D17: no
              // reprovenance); address mode only (checked above)
              if (!ptrSorted())
                return in(0);
              ++numPtrFromInt;
              return Expr(mk<PTR_FROM_INT>(in(0)));
            })
            // a pointer's own bits, split / reassembled (PtrSort.md D2)
            .Case([&](AddrToBitsOp) {
              return ptrSorted() ? mk<PTR_TO_BITS>(in(0)) : in(0);
            })
            .Case([&](AddrFromBitsOp fb) {
              if (!ptrSorted())
                return in(0);
              if (companionNeeded.value_or(false))
                if (Expr r = companionLoad(fb, in(0), ctx))
                  return r;
              return Expr(mk<PTR_FROM_BITS>(in(0)));
            })
            .Case([&](LoadOp) {
              Expr r = memLoad(in(0), in(1), unitsOf(op), ctx);
              // a cell holds bits: a loaded address is a pointer built
              // from them (PTR_FROM_BITS; S2e, as the wide path, S1.6b)
              if (r && ptrSorted() && isa<AddrType>(op.getResult(0).getType()))
                r = mk<PTR_FROM_BITS>(r);
              return r;
            })
            .Case([&](StoreOp) {
              Expr v = in(2);
              if (ptrSorted() && isa<AddrType>(op.getOperand(2).getType()))
                v = mk<PTR_TO_BITS>(v); // a stored address: its bits
              return memStore(in(0), in(1), v, unitsOf(op), ctx);
            })
            .Default([](Operation *) { return Expr(); });
    if (!val)
      return op.emitError("sea-vc: unsupported op");
    // Ops are total, so definitions need no path condition.
    ctx.write(mkSymbReg(op.getResult(0)), val);
  }
  runDefHook();
  settleKeyed(ctx);
  return success();
}

/// The mirror check of the companion exemption: a keyed access is exempt
/// only if a program access of this
/// block has the same address term (a range: the same (d, n)). The proof
/// rests on that: the data access's A-INB condition, under the same path
/// condition, implies the 1-unit facts the rules on the companion memory
/// use. An unmirrored one is a program access after all (its own 1-unit
/// condition, counted), with a warning (vc.seam-companion-unmirrored).
void SeaOpSem::settleKeyed(OpSemContext &ctx) {
  auto fallback = [&](Expr inb, Location loc) {
    ++seamRw->stats().rules["companion.unmirrored"];
    diag::warn("vc.seam-companion-unmirrored",
               "a keyed (companion) access with no program access at the "
               "same address in its block: counted as a program access",
               loc);
    ++inbAccesses;
    if (!isOpX<TRUE>(inb))
      inbSeen.insert(mk<AND>(ctx.getPathCond(), boolop::lneg(inb)));
  };
  for (const auto &[p, loc] : pendingKeyed)
    if (!blockAddrs.count(p)) {
      bool violated = false;
      fallback(seamRw->rules().inBounds(p, 1, violated), loc);
      if (violated)
        noteInbViolation(loc, "companion", [&] {
          return seamRw->rules().explainViolation(p, 1, false);
        });
    }
  for (const auto &[dv, nv, loc] : pendingKeyedRanges)
    if (!blockRanges.count({dv, nv})) {
      bool violated = false;
      Expr d = seamRw->rewrite(lookup(dv, ctx)),
           n = seamRw->rewrite(lookup(nv, ctx));
      fallback(seamRw->rules().rangeInBounds(d, n, violated), loc);
      if (violated)
        noteInbViolation(loc, "companion", [&] {
          return seamRw->rules().explainViolation(d, 0, true, n);
        });
    }
  pendingKeyed.clear();
  pendingKeyedRanges.clear();
}

/// sea.ptr.index. The lowering's arithmetic (sea-lower-ptr-index, then the
/// encoding of sext / mul / add / addr.add): base + (((sext(x_1) * s_1 +
/// sext(x_2) * s_2) + ...) + c), the constant only when nonzero, base + c with
/// no index. Without SeaM that is the term. With SeaM it is a PTR_INDEX node
/// (ExprOpPtr.hh) over the sign-extended indices, K = 0, the operator from
/// the flags: inbounds / nusw -> PTR_INDEX_NUSW, else nuw -> PTR_INDEX_NUW
/// (both flags: NUSW; [OPEN] an operator with both promises), else
/// PTR_INDEX. Strides and c: signed for NUSW, unsigned n-bit for NUW, mod
/// 2^n for PTR_INDEX. The rules and the boundary read a node through its flat
/// form (SeaMRules.hh), so the VC is the lowered program's.
Expr SeaOpSem::ptrIndexTerm(PtrIndexOp p, OpSemContext &ctx) {
  unsigned n = bitWidthOf(p.getBase().getType());
  Expr base = lookup(p.getBase(), ctx);
  auto cst = [&](int64_t v) {
    APInt a(64, uint64_t(v), /*isSigned=*/true);
    return bvnum(n < 64 ? a.trunc(n) : a);
  };
  std::vector<std::pair<Expr, mpz_class>> idx;
  Expr off;
  bool nusw = p.getInbounds() || p.getNusw(), nuw = p.getNuw();
  auto value = [&](int64_t v) -> mpz_class {
    APInt a(64, uint64_t(v), /*isSigned=*/true);
    if (n < 64)
      a = a.trunc(n);
    mpz_class u = expr::op::bv::toMpz(bvnum(a)); // unsigned, < 2^n
    if (nusw && a.isNegative()) {
      mpz_class m;
      mpz_ui_pow_ui(m.get_mpz_t(), 2, n);
      u = u - m;
    }
    return u;
  };
  for (auto [k, iv] : llvm::enumerate(p.getIndices())) {
    Expr x = lookup(iv, ctx);
    if (bitWidthOf(iv.getType()) < n)
      x = bv::sext(x, n);
    Expr t = mk<BMUL>(x, cst(p.getStrides()[k]));
    off = off ? Expr(mk<BADD>(off, t)) : t;
    idx.push_back({x, value(p.getStrides()[k])});
  }
  if (!off)
    off = cst(p.getOffset());
  else if (p.getOffset() != 0)
    off = mk<BADD>(off, cst(p.getOffset()));
  Expr arith = mk<BADD>(base, off);
  if (!ptrSorted())
    return arith;
  // kept alive with the rewriter (its sum order is by id); without it the
  // boundary builds the same term from the node (ExprOpPtr.hh arith)
  if (seamRw)
    ptrIndexArith.push_back(arith);
  op::PtrOpKind kind = nusw  ? op::PtrOpKind::PTR_INDEX_NUSW
                       : nuw ? op::PtrOpKind::PTR_INDEX_NUW
                             : op::PtrOpKind::PTR_INDEX;
  return op::ptr::ptrIndex(kind, base, idx, value(p.getOffset()),
                           mpz_class(0L));
}

/// addr.add. With the stride rule's K writers (op::ptr::addressPlus, sea-vc
/// --ack-stride), a numeral added to a promised PTR_INDEX node (or an ite of
/// them) is that node with K + k, k signed (sea-split-wide-access makes the
/// bytes of a wide access so; PtrIndex.md 0b). Else bvadd.
Expr SeaOpSem::addrAdd(Expr a, Expr o) {
  // PtrSort rev 5, S1.1: with SeaM every pointer addition is a node, the
  // only pointer-arithmetic constructor (PtrIndex.md rev 6, section 0)
  return ptrSorted() ? op::ptr::addressPlus(a, o) : Expr(mk<BADD>(a, o));
}

/// memset's value (one unit) as a cell of \p memTy: the unit repeated
/// (word layouts: k copies; keyed: the cell's width in units).
Expr SeaOpSem::cellOfUnit(Expr unit, MemType memTy) {
  Expr v = unit;
  for (unsigned i = 1; i < memTy.getUnitCopies(); ++i)
    v = bv::concat(unit, v);
  return v;
}

/// Lengths up to this many cells are expanded into store chains; above it
/// the encoding refuses (the formula would be huge).
static constexpr uint64_t kMaxExpandedCells = 1 << 16;
/// Above this many cells, expansion works but warns (large formulas).
static constexpr uint64_t kWarnExpandedCells = 1024;

/// A-INB for the range [d, d + n) of a memory operation (with SeaM): the
/// range is an access (counted, its condition recorded as for a load or a
/// store); a numeral length that does not fit its object is a definite
/// violation. n = 0 is no access.
/// Is the current op's memory keyed (metadata: fat-pointer companions, a
/// cell per address, no objects)? Such an access is not a program access:
/// it states no A-INB condition and is not counted. The rules on its memory
/// may still separate addresses by object (premise A-INB): the facts are
/// the program's, stated by the data access at the same address, which
/// sea-lower-fat-pointers emits next to every companion access.
bool SeaOpSem::keyedAccess() const {
  return curOp && curOp->getNumOperands() &&
         isa<MemType>(curOp->getOperand(0).getType()) &&
         cast<MemType>(curOp->getOperand(0).getType()).isKeyed();
}

void SeaOpSem::accessRange(Expr d, Expr n, OpSemContext &ctx) {
  // a companion range (memset / memmove of a keyed memory) mirrors the data
  // range op, which states the condition
  if (keyedAccess()) {
    ++seamRw->stats().rules["companion.access"];
    return; // its mirror is checked on the op's operands (execRangeOp)
  }
  ++inbAccesses;
  bool violated = false;
  Expr inb = seamRw->rules().rangeInBounds(d, n, violated);
  if (violated)
    noteInbViolation(curOp->getLoc(), curOp->getName().stripDialect(), [&] {
      return seamRw->rules().explainViolation(d, 0, true, n);
    });
  if (!isOpX<TRUE>(inb))
    inbSeen.insert(mk<AND>(ctx.getPathCond(), boolop::lneg(inb)));
}

/// Memory-operation terms (docs/design/SeaM-memops.md): with SeaM, a memset /
/// memmove / memhavoc of a byte (index-unit) memory is one term, mset / mcopy /
/// mhavoc (MemOps.hh), whatever its length: no cells, no bound. The rules
/// decide a read over it once (SeaMRules.hh rowMemOp); the boundary translates
/// what is left by the read equations.
void SeaOpSem::execRangeTerm(Operation &op, Value lenV, OpSemContext &ctx) {
  auto in = [&](unsigned i) { return lookup(op.getOperand(i), ctx); };
  seamRw->noteMemOps();
  trailAccess("addr", 0);
  Expr n = seamRw->rewrite(lookup(lenV, ctx));
  Expr mem = in(0);
  Expr dst = seamRw->rewrite(in(1));
  accessRange(dst, n, ctx);
  Expr res;
  if (isa<MemSetOp>(op)) {
    res = op::array::memSet(
        mem, dst, n,
        cellOfUnit(in(2), cast<MemType>(op.getOperand(0).getType())));
  } else if (isa<MemMoveOp>(op)) {
    // the source as it was before the op: its own memory version
    Expr from = seamRw->rewrite(in(3));
    accessRange(from, n, ctx);
    res = op::array::memCopy(mem, in(2), dst, from, n);
  } else {
    auto mh = cast<MemHavocOp>(op);
    auto memTy = cast<MemType>(op.getOperand(0).getType());
    Expr h = bind::mkConst(
        mkTerm<std::string>("nd." + std::to_string(mh.getSite()), ef),
        sort::arrayTy(memIndexSort(memTy, ptrSorted(), ef),
                      bv::bvsort(memTy.getCell().getWidth(), ef)));
    havocs[&op] = h;
    res = op::array::memHavoc(mem, h, dst, n);
  }
  // the term's own rules (addr.prune-size0 over its ranges); the A-INB of the
  // ranges was recorded above, from the unpruned addresses
  trailAccess("range", 1);
  res = seamRw->rewrite(res);
  ctx.write(mkSymbReg(op.getResult(0)), res);
}

LogicalResult SeaOpSem::execRangeOp(Operation &op, OpSemContext &ctx) {
  Value lenV = llvm::TypeSwitch<Operation *, Value>(&op)
                   .Case([](MemSetOp o) { return o.getLen(); })
                   .Case([](MemMoveOp o) { return o.getLen(); })
                   .Case([](MemHavocOp o) { return o.getLen(); });
  // the mirror check of companion ranges (settleKeyed), on the operands
  if (seamMode) {
    SmallVector<Value, 2> addrs{op.getOperand(1)};
    if (auto mv = dyn_cast<MemMoveOp>(op))
      addrs.push_back(mv.getSrcAddr());
    if (auto mh = dyn_cast<MemHavocOp>(op))
      addrs = {mh.getAddr()};
    for (Value a : addrs) {
      if (keyedAccess())
        pendingKeyedRanges.push_back({a, lenV, op.getLoc()});
      else
        blockRanges.insert({a, lenV});
    }
  }
  if (lambdaMode) {
    // docs/design/Lambda.md: lambda i. ite((i - d) <u n, <hit>, M(i)), the
    // circular membership of SeaM-memops.md; any length, symbolic too.
    // A word memory (sea-widen-memory: aligned k-byte cells keyed by their
    // first byte) has k-aligned addresses and a constant length that is a
    // multiple of k (the pass's contract), so the same membership over cell
    // addresses is exact; memset's byte is repeated per word.
    auto in = [&](unsigned i) { return lookup(op.getOperand(i), ctx); };
    Expr mem = in(0), d = in(1), n = lookup(lenV, ctx);
    Expr idx = curIndexSort(&op, ef);
    Expr res;
    if (isa<MemSetOp>(op)) {
      Expr v = cellOfUnit(in(2), cast<MemType>(op.getOperand(0).getType()));
      res = mkLambda(idx, [&](Expr i) {
        return mk<ITE>(mk<BULT>(mk<BSUB>(i, d), n), v, bind::fapp(mem, i));
      });
    } else if (isa<MemMoveOp>(op)) {
      // operands: memory, destination, source memory, source address
      Expr src = in(2), s = in(3);
      res = mkLambda(idx, [&](Expr i) {
        Expr k = mk<BSUB>(i, d);
        return mk<ITE>(mk<BULT>(k, n), bind::fapp(src, mk<BADD>(s, k)),
                       bind::fapp(mem, i));
      });
    } else {
      auto mh = cast<MemHavocOp>(op);
      auto memTy = cast<MemType>(op.getOperand(0).getType());
      Expr h = bind::mkConst(
          mkTerm<std::string>("nd." + std::to_string(mh.getSite()), ef),
          sort::arrayTy(memIndexSort(memTy, ptrSorted(), ef),
                        bv::bvsort(memTy.getCell().getWidth(), ef)));
      havocs[&op] = h;
      res = mkLambda(idx, [&](Expr i) {
        return mk<ITE>(mk<BULT>(mk<BSUB>(i, d), n), op::array::select(h, i),
                       bind::fapp(mem, i));
      });
    }
    ctx.write(mkSymbReg(op.getResult(0)), res);
    return success();
  }
  if (seamMode && unitsOf(op) == 1) {
    execRangeTerm(op, lenV, ctx);
    return success();
  }
  auto lenOp = lenV.getDefiningOp<ConstOp>();
  std::optional<llvm::APInt> lenNum;
  if (lenOp) {
    lenNum = cast<IntegerAttr>(lenOp.getValue()).getValue();
  } else if (seamMode) {
    // --seam does not run sea-expand-memset, which folds lengths computed
    // from constants: a numeral after normalization is a constant length
    trailAccess("addr", 0);
    Expr n = seamRw->rewrite(lookup(lenV, ctx));
    unsigned nw;
    if (bv::isBvNum(n, nw))
      lenNum = toAPInt(nw, seam::Rules::numVal(n));
  }
  if (!lenNum) {
    diag::error("vc.mem-op-symbolic-length",
                op.getName().getStringRef() +
                    " with a symbolic length: the array encoding cannot "
                    "express it (needs an expansion pass or the lambda "
                    "encoding)",
                op.getLoc());
    return failure();
  }
  llvm::APInt len = *lenNum;
  // Lengths are in index units; a word layout has k-unit cells, keyed by
  // their first byte (the array encoding is the same for aligned and
  // unaligned words: alignment is proved by the pass that chose the layout,
  // or assumed, for unaligned).
  auto memTy = cast<MemType>(op.getOperand(0).getType());
  uint64_t units = memTy.getCellUnits();
  if (len.urem(units) != 0) {
    diag::error("vc.mem-op-partial-word",
                op.getName().getStringRef() + " of " +
                    llvm::toString(len, 10, false) + " bytes is not whole " +
                    llvm::Twine(units) + "-byte cells",
                op.getLoc());
    return failure();
  }
  len = len.udiv(units);
  if (len.ugt(kMaxExpandedCells)) {
    diag::error("vc.mem-op-too-long",
                op.getName().getStringRef() + " of " +
                    llvm::toString(len, 10, false) +
                    " cells is too long to expand into stores (limit " +
                    llvm::Twine(kMaxExpandedCells) + ")",
                op.getLoc());
    return failure();
  }
  uint64_t n = len.getZExtValue();
  if (n > kWarnExpandedCells)
    diag::warn("vc.mem-op-expanded-large",
               op.getName().getStringRef() + " of " + llvm::Twine(n) +
                   " cells expanded into " + llvm::Twine(n) + " stores",
               op.getLoc());

  auto in = [&](unsigned i) { return lookup(op.getOperand(i), ctx); };
  unsigned w = len.getBitWidth();
  auto at = [&](Expr base, uint64_t k) {
    k *= units;
    return k == 0 ? base : addrAdd(base, numeral(llvm::APInt(w, k)));
  };

  Expr mem = in(0), dst = in(1);
  if (auto ms = dyn_cast<MemSetOp>(op)) {
    // the value is one byte (one unit); a word or keyed cell is copies of it
    Expr v = cellOfUnit(in(2), memTy);
    for (uint64_t k = 0; k < n; ++k)
      mem = memStore(mem, at(dst, k), v, units, ctx);
  } else if (isa<MemMoveOp>(op)) {
    // Reads come from the source version as it was before the op.
    Expr src = in(2), from = in(3);
    for (uint64_t k = 0; k < n; ++k)
      mem = memStore(mem, at(dst, k), memLoad(src, at(from, k), units, ctx),
                     units, ctx);
  } else {
    auto mh = cast<MemHavocOp>(op);
    Expr h = bind::mkConst(
        mkTerm<std::string>("nd." + std::to_string(mh.getSite()), ef),
        sort::arrayTy(memIndexSort(memTy, ptrSorted(), ef),
                      bv::bvsort(memTy.getCell().getWidth(), ef)));
    havocs[&op] = h;
    for (uint64_t k = 0; k < n; ++k)
      mem = memStore(mem, at(dst, k), memLoad(h, at(dst, k), units, ctx), units,
                     ctx);
  }
  ctx.write(mkSymbReg(op.getResult(0)), mem);
  return success();
}

void SeaOpSem::execBr(Block &src, Block &dst, OpSemContext &ctx) {
  Operation *term = src.getTerminator();
  if (auto cbr = dyn_cast<CondBrOp>(term)) {
    // The form check guarantees distinct targets.
    Expr c = toBool(lookup(cbr.getCondition(), ctx));
    ctx.addScopedSide(cbr.getTrueDest() == &dst ? c : boolop::lneg(c));
  }
  // sea.br: the edge has no condition.
}

SmallVector<Expr> SeaOpSem::execPhi(Block &dst, Block &src, OpSemContext &ctx) {
  auto br = cast<BranchOpInterface>(src.getTerminator());
  SmallVector<Expr> vals;
  for (unsigned i = 0, e = br->getNumSuccessors(); i < e; ++i) {
    if (br->getSuccessor(i) != &dst)
      continue;
    for (Value v : br.getSuccessorOperands(i).getForwardedOperands())
      vals.push_back(lookup(v, ctx));
    break;
  }
  return vals;
}

//===----------------------------------------------------------------------===//
// The pointer companion (PtrSort S6, rev 5 2.4; see OpSem.h ptrCompanion)
//===----------------------------------------------------------------------===//

Expr SeaOpSem::storedPointer(StoreOp st, OpSemContext &ctx) {
  Value v = st.getValue();
  if (isa<AddrType>(v.getType()))
    return lookup(v, ctx);
  // the offset-0 byte of a split pointer store (sea-split-wide-access:
  // extract(addr.to_bits(p), 0) at the store's address)
  if (auto ex = v.getDefiningOp<ExtractOp>())
    if (ex.getLow() == 0)
      if (auto tb = ex.getOperand().getDefiningOp<AddrToBitsOp>())
        return lookup(tb.getOperand(), ctx);
  return Expr();
}

Expr SeaOpSem::companionOf(Value mem, OpSemContext &ctx) {
  auto memTy = dyn_cast<MemType>(mem.getType());
  if (!memTy || !isa<AddrType>(memTy.getIndex()))
    return Expr();
  // the chain back to a value with a companion or a root, then forward
  // (iterative: VC memories are deep chains)
  SmallVector<Operation *> chain;
  Value v = mem;
  Expr c;
  while (true) {
    auto it = companions.find(v);
    if (it != companions.end()) {
      c = it->second;
      break;
    }
    Operation *def = v.getDefiningOp();
    if (def && isa<StoreOp, MemSetOp, MemHavocOp, MemMoveOp>(def)) {
      chain.push_back(def);
      v = def->getOperand(0);
      continue;
    }
    // a root: a memory constant, a block argument no join reached, an
    // unrecognized producer: fresh (any companion is exact, by the guard)
    Expr ps = op::ptr::ptrsort(memTy.getIndexWidth(), ef);
    c = bind::mkConst(mkTerm<std::string>("ptrc." + names.name(v).str() + "." +
                                              std::to_string(numCompanionFresh),
                                          ef),
                      sort::arrayTy(ps, ps));
    ++numCompanionFresh;
    companions[v] = c;
    break;
  }
  for (Operation *op : llvm::reverse(chain)) {
    if (auto st = dyn_cast<StoreOp>(op)) {
      if (Expr p = storedPointer(st, ctx)) {
        c = op::array::store(c, lookup(st.getAddr(), ctx), p);
        ++numCompanionStores;
      }
    } else if (auto mv = dyn_cast<MemMoveOp>(op)) {
      // operands: memory, destination, source memory, source address, length
      Expr src = companionOf(op->getOperand(2), ctx);
      if (src && seamMode && unitsOf(*op) == 1) {
        c = mknary<MEM_COPY>(ExprVector{c, src, lookup(op->getOperand(1), ctx),
                                        lookup(op->getOperand(3), ctx),
                                        lookup(mv.getLen(), ctx)});
        ++numCompanionCopies;
      }
    }
    // memset / memhavoc / an integer store: the companion is kept (a stale
    // cell fails the guard)
    companions[op->getResult(0)] = c;
  }
  return c;
}

LogicalResult SeaOpSem::joinCompanions(Block &dst, ArrayRef<Block *> preds,
                                       ArrayRef<Expr> edges, OpSemContext &ctx,
                                       JoinCases *cases) {
  if (!ptrCompanion() || !companionNeeded.value_or(false))
    return success();
  for (BlockArgument arg : dst.getArguments()) {
    if (!isa<MemType>(arg.getType()))
      continue;
    SmallVector<Expr> vals;
    for (Block *src : preds) {
      auto br = cast<BranchOpInterface>(src->getTerminator());
      for (unsigned i = 0, e = br->getNumSuccessors(); i < e; ++i) {
        if (br->getSuccessor(i) != &dst)
          continue;
        Value in = br.getSuccessorOperands(i)
                       .getForwardedOperands()[arg.getArgNumber()];
        vals.push_back(companionOf(in, ctx));
        break;
      }
    }
    if (vals.empty() || !vals.back())
      continue;
    if (cases) {
      // the same join as below, through a cases term (--vc-join-cases)
      std::string why;
      Expr val = cases->join(edges, vals, JoinKind::Companion, why);
      if (!val) {
        diag::error("vc.cases-join", "join of a pointer companion: " + why,
                    arg.getLoc());
        return failure();
      }
      companions[arg] = val;
      continue;
    }
    // VCGen's ite join: the last edge is the default
    Expr val = vals.back();
    for (unsigned j = vals.size() - 1; j > 0; --j)
      if (vals[j - 1] != val)
        val = boolop::lite(edges[j - 1], vals[j - 1], val);
    companions[arg] = val;
  }
  return success();
}

Expr SeaOpSem::companionLoad(AddrFromBitsOp fb, Expr bits, OpSemContext &ctx) {
  // the split load's first byte (sea-split-wide-access: concat(byte_k,
  // concat(..., load(M, q))))
  Value v = fb.getOperand();
  while (auto cc = v.getDefiningOp<ConcatOp>())
    v = cc.getOperand(1);
  auto ld = v.getDefiningOp<LoadOp>();
  if (!ld || v == fb.getOperand())
    return Expr();
  Expr comp = companionOf(ld.getMem(), ctx);
  if (!comp)
    return Expr();
  ++numCompanionLoads;
  Expr c = op::array::select(comp, lookup(ld.getAddr(), ctx));
  if (ptrIntAssume) {
    ++numCompanionAssumed; // uses: A-PTRINT
    return c;
  }
  Expr guard = mk<EQ>(mk<PTR_TO_BITS>(c), bits);
  if (ptrIntCheck) {
    ++numCompanionChecked;
    ctx.addScopedGuarantee(guard);
  }
  return mk<ITE>(guard, c, mk<PTR_FROM_BITS>(bits));
}

} // namespace sea
