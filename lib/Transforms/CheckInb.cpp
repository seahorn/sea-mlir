// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- CheckInb.cpp - the A-INB discharge job's instrumentation ---------===//
//
// docs/design/CheckInb.md. A-INB ("every access through a pointer lies in
// the object the pointer came from", SeaM.md) implies the pointer premises
// of `sea-vc --seam` (A-SEP, A-NULL, A-NOWRAP). This pass is the
// instrumentation of the job that discharges it: before every access of an
// abstract program it asserts that the accessed span lies in the pointer's
// object,
//     load / store of k units at p             assert deref(p, k)
//     memset / memhavoc of n units at p        assert deref(p, n)
//     memmove of n units from s to d           assert deref(d, n)
//                                              assert deref(s, n)
// with deref = sea.ptr.dereferenceable: p is not null, off <=u size and
// n <=u size - off (unsigned, at the pointer width; size - off cannot wrap
// once off <=u size holds; the design note has the argument). A zero-length
// range is not an access: a constant length of 0 gets no check, a symbolic
// one gets `n == 0 || deref(p, n)`. A memhavoc of the null pointer itself
// is a no-op (the open decision of sea-guard-null-memhavoc, sea-vc
// --memhavoc-null=noop, the default): with memhavoc-null-noop (default
// true) the check of a memhavoc whose pointer may be null (the test of
// sea-guard-null-memhavoc) also passes when p is null (`p == null ||
// ...`); with false it is an access like any other.
//
// The object of an access is its pointer's provenance (fat pointers, as
// SeaHorn's fpf), not whatever object the address lands in. A pointer made
// by ptr.from_int, ptr.null, func_addr or ptr.nondet has an object of size
// 0, so every check of a positive-length access through it fails: stricter
// than LLVM, on purpose. The pass warns (check-inb.size0-object) when an
// access's pointer comes, through ptr.add / ptr.index bases, from one of
// those: its check fails whenever the access executes.
//
// Static discharge, tier (a): an access of constant length through a
// pointer whose offset in its object is a constant and whose object has a
// constant size (an alloca / malloc of constant size, or a global, reached
// through ptr.add of constant offsets and ptr.index of constant indices)
// gets no assert when the check is true for those constants: the same
// unsigned comparison as above, at the pointer width (the offset is the sum
// of the steps modulo 2^w, as the fat-pointer offset companion computes
// it). It never removes a check it has not proved; with static=false every
// check stays (the verdict must not change).
//
// The program's own asserts are dropped: the job checks memory safety on
// every execution, not the program's property; its assumes stay. A tagged
// assert (sea-vc --reach) leaves its reach-site marker, as in
// sea-check-align. A sea.box.all must be lowered first (sea-lower-box-all,
// as sea-vc --check-inb does): its body's loads then sit at the box's
// place as ordinary loads at an arbitrary index of the box (or at every
// index, expanded), and each gets a check. That is stricter than checking
// at the claim's assert (a read is checked even when an earlier conjunct
// of the claim already fails), which a discharge check may be. A box that
// reaches this pass is an error (check-inb.unsupported): its loads would go
// unchecked while --seam rests on A-SEP for them. Every inserted assert carries
// kInbCheckAttr, its id (0, 1, ... in program order of the checks, statically
// discharged ones included). sea-merge-asserts then makes the BMC form, in
// which each check's continuation starts with `assume` of it: an access after a
// check runs only with the check true.
//
// Run on the abstract program (pointers still know their objects), before
// sea-lower-fat-pointers, which lowers the checks to the offset and size
// companions, and before sea-drop-fresh-memhavoc and
// sea-guard-null-memhavoc (the memhavocs they remove or guard are checked
// first).
//
// Contract.
//   Accepts: an abstract program; every access in the program body has a
//     pointer address (!sea.ptr), and no sea.box.all is left (lowered
//     first); otherwise error check-inb.unsupported.
//   Produces: one assert per access span not discharged statically, no
//     original assert, and the counters on the program
//     (kCheckInbStatsAttr): checks (spans checked, discharged ones
//     included), static (discharged by tier (a)), left (asserts inserted),
//     zero-length (constant length 0: no check), and null-noop (when not
//     0: the memhavoc checks, of a pointer that may be null, that pass
//     for the null pointer itself under memhavoc-null-noop; the job's
//     result rests on that decision).
//
//===----------------------------------------------------------------------===//

#include "PassUtils.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Support/Diag.h"
#include "sea/Transforms/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/Pass/Pass.h"

using namespace mlir;
using llvm::APInt;

namespace sea {
namespace {

class CheckInbPass
    : public PassWrapper<CheckInbPass, OperationPass<ProgramOp>> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckInbPass)

  CheckInbPass() = default;
  CheckInbPass(const CheckInbPass &o) : PassWrapper(o) {}
  CheckInbPass(bool staticDischarge, bool memhavocNullNoop) {
    this->staticDischarge = staticDischarge;
    this->memhavocNullNoop = memhavocNullNoop;
  }

  StringRef getArgument() const final { return "sea-check-inb"; }
  StringRef getDescription() const final {
    return "Assert before every load / store / memset / memmove / memhavoc "
           "that its span lies in its pointer's object (the A-INB discharge "
           "job); drop the program's asserts";
  }

  void runOnOperation() override;

  Option<bool> staticDischarge{
      *this, "static",
      llvm::cl::desc("Tier (a) static discharge: no assert for a check "
                     "proved true with constant offset, size and length"),
      llvm::cl::init(true)};
  Option<bool> memhavocNullNoop{
      *this, "memhavoc-null-noop",
      llvm::cl::desc("A memhavoc of the null pointer is a no-op, not an "
                     "access (sea-vc --memhavoc-null=noop)"),
      llvm::cl::init(true)};
};

/// The provenance of a pointer as tier (a) sees it: the op that made its
/// object and the offset of the pointer in it (modulo 2^w); none when a
/// step is not constant or the chain reaches something else.
struct ConstPtr {
  Operation *object = nullptr;
  APInt offset;
};

/// The root of p's chain of ptr.add / ptr.index bases.
static Operation *rootOf(Value p) {
  while (Operation *def = p.getDefiningOp()) {
    if (auto pa = dyn_cast<PtrAddOp>(def))
      p = pa.getPtr();
    else if (auto pi = dyn_cast<PtrIndexOp>(def))
      p = pi.getBase();
    else
      return def;
  }
  return nullptr;
}

static std::optional<ConstPtr> constPtr(Value p) {
  unsigned w = cast<PtrType>(p.getType()).getWidth();
  APInt off(w, 0);
  while (Operation *def = p.getDefiningOp()) {
    if (auto pa = dyn_cast<PtrAddOp>(def)) {
      std::optional<APInt> c = foldConstant(pa.getOffset());
      if (!c)
        return std::nullopt;
      // the offset operand has the pointer width (verifier); sext keeps
      // the value modulo 2^w either way
      off += c->sextOrTrunc(w);
      p = pa.getPtr();
    } else if (auto pi = dyn_cast<PtrIndexOp>(def)) {
      // sext(i_k) * s_k + c, modulo 2^w (PtrIndex.md; the flags never
      // change the value)
      for (auto [idx, stride] :
           llvm::zip_equal(pi.getIndices(), pi.getStrides())) {
        std::optional<APInt> c = foldConstant(idx);
        if (!c)
          return std::nullopt;
        off += c->sextOrTrunc(w) * APInt(w, stride, /*isSigned=*/true,
                                         /*implicitTrunc=*/true);
      }
      off += APInt(w, pi.getOffset(), /*isSigned=*/true,
                   /*implicitTrunc=*/true);
      p = pi.getBase();
    } else if (isa<AllocaOp, MallocOp, GlobalOp>(def)) {
      return ConstPtr{def, off};
    } else {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

/// The size of a tier-(a) object at width w, if constant.
static std::optional<APInt> constSize(Operation *object, unsigned w) {
  if (auto g = dyn_cast<GlobalOp>(object))
    return APInt(w, g.getSize(), /*isSigned=*/false, /*implicitTrunc=*/true);
  std::optional<APInt> c = foldConstant(object->getOperand(0));
  if (!c)
    return std::nullopt;
  return c->zextOrTrunc(w);
}

void CheckInbPass::runOnOperation() {
  ProgramOp program = getOperation();
  MLIRContext *ctx = &getContext();
  OpBuilder b(ctx);
  auto i1 = IntegerType::get(ctx, 1);
  SmallVector<AssertOp> original;
  program.walk([&](AssertOp a) { original.push_back(a); });

  uint64_t checks = 0, discharged = 0, left = 0, zeroLength = 0,
           nullNoopChecks = 0;
  bool ok = true;

  // one span: `len` units at `p` (a constant length: `constLen`)
  auto check = [&](Operation *op, Value p, Value len,
                   std::optional<APInt> constLen, bool nullNoop) {
    Location loc = op->getLoc();
    auto pt = dyn_cast<PtrType>(p.getType());
    if (!pt) {
      diag::error("check-inb.unsupported",
                  "sea-check-inb: an access whose address is not an abstract "
                  "pointer (run it on the abstract program, before "
                  "placement)",
                  loc);
      ok = false;
      return;
    }
    unsigned w = pt.getWidth();
    if (constLen && constLen->isZero()) {
      ++zeroLength;
      return;
    }
    uint64_t id = checks++;
    // (a memhavoc of the null pointer itself passes under null-noop)
    if (Operation *root = rootOf(p);
        root && isa<PtrFromIntOp, PtrNullOp, FuncAddrOp, PtrNondetOp>(root) &&
        !(nullNoop && p.getDefiningOp<PtrNullOp>()))
      diag::warn("check-inb.size0-object",
                 "the access's pointer comes from " +
                     root->getName().getStringRef() +
                     ", an object of size 0: its check fails whenever the "
                     "access executes",
                 loc);
    if (staticDischarge && constLen) {
      if (std::optional<ConstPtr> cp = constPtr(p))
        if (std::optional<APInt> size = constSize(cp->object, w)) {
          APInt n = constLen->zextOrTrunc(w);
          // the same comparison as sea.ptr.dereferenceable: unsigned, at
          // the pointer width; the object is not null
          if (constLen->getActiveBits() <= w && cp->offset.ule(*size) &&
              n.ule(*size - cp->offset)) {
            ++discharged;
            return;
          }
        }
    }
    b.setInsertionPoint(op);
    auto iW = IntegerType::get(ctx, w);
    if (!len)
      len = ConstOp::create(b, loc, iW,
                            IntegerAttr::get(iW, constLen->zextOrTrunc(w)));
    Value cond = PtrDereferenceableOp::create(b, loc, i1, p, len);
    if (!constLen) {
      Value zero = ConstOp::create(b, loc, iW, IntegerAttr::get(iW, 0));
      Value empty = ICmpOp::create(b, loc, ICmpPredicate::eq, len, zero);
      cond = OrOp::create(b, loc, i1, empty, cond);
    }
    // a pointer that may be null (the test of sea-guard-null-memhavoc): the
    // check rests on memhavoc(null) = no-op (sea-vc reports it); one that
    // cannot be null needs no disjunct
    if (nullNoop && mayBeNullPointer(p)) {
      ++nullNoopChecks;
      Value null = PtrNullOp::create(b, loc, pt);
      Value isNull = PtrCmpOp::create(b, loc, ICmpPredicate::eq, p, null);
      cond = OrOp::create(b, loc, i1, isNull, cond);
    }
    auto a = AssertOp::create(b, loc, cond);
    a->setAttr(kInbCheckAttr, b.getI64IntegerAttr(id));
    ++left;
  };
  // the units of a value of type t in memory m
  auto unitsOf = [&](Type t, MemType m) {
    unsigned unit = bitWidthOf(m.getUnitType());
    unsigned bits = bitWidthOf(t);
    return APInt(64, (bits + unit - 1) / unit);
  };
  auto lenOf = [&](Value len) -> std::pair<Value, std::optional<APInt>> {
    if (std::optional<APInt> c = foldConstant(len))
      return {Value(), c};
    return {len, std::nullopt};
  };

  // the loads of a box's body are checked only once the box is lowered
  WalkResult boxes = program.walk([&](BoxAllOp box) {
    diag::error("check-inb.unsupported",
                "sea-check-inb: a sea.box.all is left; lower it first "
                "(sea-lower-box-all), so that the loads of its body are "
                "checked",
                box.getLoc());
    return WalkResult::interrupt();
  });
  if (boxes.wasInterrupted())
    return signalPassFailure();

  for (Block &block : program.getBody()) {
    for (Operation &opRef : llvm::make_early_inc_range(block)) {
      Operation *op = &opRef;
      if (auto ld = dyn_cast<LoadOp>(op)) {
        check(op, ld.getAddr(), Value(),
              unitsOf(ld.getResult().getType(), ld.getMem().getType()), false);
      } else if (auto st = dyn_cast<StoreOp>(op)) {
        check(op, st.getAddr(), Value(),
              unitsOf(st.getValue().getType(), st.getMem().getType()), false);
      } else if (auto ms = dyn_cast<MemSetOp>(op)) {
        auto [l, c] = lenOf(ms.getLen());
        check(op, ms.getAddr(), l, c, false);
      } else if (auto mv = dyn_cast<MemMoveOp>(op)) {
        auto [l, c] = lenOf(mv.getLen());
        check(op, mv.getAddr(), l, c, false);
        check(op, mv.getSrcAddr(), l, c, false);
      } else if (auto mh = dyn_cast<MemHavocOp>(op)) {
        auto [l, c] = lenOf(mh.getLen());
        check(op, mh.getAddr(), l, c, memhavocNullNoop);
      }
    }
  }
  if (!ok)
    return signalPassFailure();

  OpBuilder mb(ctx);
  for (AssertOp a : original) {
    // the top of a tagged assert stays a reach target (sea-vc --reach)
    if (Attribute site = a->getAttr(kReachSiteAttr)) {
      mb.setInsertionPoint(a);
      createReachSiteMarker(mb, a.getLoc(), a->getName().getStringRef(), site);
    }
    a.erase();
  }

  auto i64 = [&](uint64_t v) { return b.getI64IntegerAttr(v); };
  program->setAttr(kCheckInbStatsAttr,
                   b.getDictionaryAttr({
                       b.getNamedAttr("checks", i64(checks)),
                       b.getNamedAttr("left", i64(left)),
                       b.getNamedAttr("static", i64(discharged)),
                       b.getNamedAttr("zero-length", i64(zeroLength)),
                   }));
  if (nullNoopChecks)
    program->setAttr(
        kCheckInbStatsAttr, DictionaryAttr::get(ctx, [&] {
          SmallVector<NamedAttribute> a(
              program->getAttrOfType<DictionaryAttr>(kCheckInbStatsAttr)
                  .getValue());
          a.push_back(b.getNamedAttr("null-noop", i64(nullNoopChecks)));
          return a;
        }()));

  // postcondition: no original assert, every assert is a check
  unsigned asserts = 0;
  bool foreign = false;
  program.walk([&](AssertOp a) {
    ++asserts;
    foreign = foreign || !a->hasAttr(kInbCheckAttr);
  });
  if (foreign || asserts != left) {
    (void)postconditionFailed("sea-check-inb",
                              "an assert that is not a bounds check", program);
    return signalPassFailure();
  }
}

} // namespace

std::unique_ptr<Pass> createCheckInbPass(bool staticDischarge,
                                         bool memhavocNullNoop) {
  return std::make_unique<CheckInbPass>(staticDischarge, memhavocNullNoop);
}

} // namespace sea
