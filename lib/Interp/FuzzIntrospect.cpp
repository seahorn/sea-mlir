// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- FuzzIntrospect.cpp - input sites and conditions over them ----------===//
//
// Static introspection for sea-run --fuzz (docs/reference/fuzzing.md, "For
// agents"): the input sites of a program (--fuzz-sites), and a blocking
// assume's condition written over them (the cond= field of fuzz.blocked).
// The condition is a bounded backward walk, not a symbolic execution: it
// stops (`?`) at memory, at block arguments with several incoming values,
// at ops with effects, and at the depth and node bounds.
//
//===----------------------------------------------------------------------===//

#include "sea/Interp/Fuzz.h"

#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <functional>
#include <map>
#include <set>

using namespace mlir;

namespace sea {

std::vector<InputSite> inputSitesOf(ProgramOp program) {
  std::vector<InputSite> out;
  program.walk([&](Operation *op) {
    InputSite s;
    s.op = op;
    if (auto n = dyn_cast<NondetOp>(op)) {
      s.site = n.getSite();
      s.callee = n.getCallee().str();
      s.width = n.getType().getIntOrFloatBitWidth();
    } else if (auto n = dyn_cast<StructNondetOp>(op)) {
      s.site = n.getSite();
      s.callee = n.getCallee().str();
      s.width = n.getBitsWidth();
    } else if (auto m = dyn_cast<MemNondetOp>(op)) {
      s.site = m.getSite();
      s.callee = m.getName().str();
      s.width = cast<MemType>(m.getType()).getCell().getWidth();
      s.memory = true;
    } else if (auto h = dyn_cast<MemHavocOp>(op)) {
      s.site = h.getSite();
      s.callee = "-";
      s.width = cast<MemType>(h.getMem().getType()).getCell().getWidth();
      s.memory = true;
    } else {
      return;
    }
    out.push_back(std::move(s));
  });
  return out;
}

std::vector<Operation *> blockingOpsOf(ProgramOp program) {
  std::vector<Operation *> out;
  program.walk([&](Operation *op) {
    if (isa<AssumeOp, UnreachableOp>(op))
      out.push_back(op);
  });
  return out;
}

namespace {

class CondWriter {
public:
  std::string render(Value v, unsigned depth);
  std::set<uint64_t> sites;

private:
  /// The one value every predecessor passes to block argument `arg`, or
  /// null (none or several; the entry block).
  static Value singleIncoming(BlockArgument arg);
  std::string site(uint64_t n) {
    sites.insert(n);
    return "s" + std::to_string(n);
  }
  unsigned nodes = 0;
};

} // namespace

/// The ops describeCondition renders, by name, with the attributes it
/// shows or that no value depends on; null: not rendered.
static const llvm::StringSet<> *allowedAttrs(Operation *op) {
  static const llvm::StringMap<llvm::StringSet<>> table = [] {
    llvm::StringMap<llvm::StringSet<>> t;
    for (const char *n :
         {"udiv",         "sdiv",        "urem",          "srem",
          "and",          "or",          "xor",           "lshr",
          "ashr",         "umin",        "umax",          "smin",
          "smax",         "bswap",       "ctpop",         "fshl",
          "fshr",         "uaddo",       "saddo",         "usubo",
          "ssubo",        "umulo",       "smulo",         "sdivo",
          "nego",         "concat",      "zext",          "sext",
          "trunc",        "select",      "struct.create", "addr.add",
          "addr.diff",    "addr.to_int", "addr.from_int", "addr.from_bits",
          "addr.to_bits", "ptr.add",     "ptr.diff",      "ptr.null"})
      t[std::string("sea.") + n] = {};
    for (const char *n : {"add", "sub", "mul", "shl"})
      t[std::string("sea.") + n] = {"nsw", "nuw"};
    for (const char *n : {"icmp", "addr.cmp", "ptr.cmp"})
      t[std::string("sea.") + n] = {"predicate"};
    t["sea.extract"] = {"low"};
    t["sea.struct.extract"] = {"index"};
    t["sea.struct.insert"] = {"index"};
    // an object's pointer: its name identifies it; size, align and label
    // do not change the value
    t["sea.global"] = {"name", "size", "align", "label"};
    t["sea.func_addr"] = {"name"};
    t["sea.ptr.index"] = {"strides", "offset", "inbounds", "nusw", "nuw"};
    return t;
  }();
  auto it = table.find(op->getName().getStringRef());
  return it == table.end() ? nullptr : &it->second;
}

/// The one value every predecessor passes to block argument `arg`, or null
/// (none or several; the entry block).
static Value singleIncomingValue(BlockArgument arg) {
  Block *b = arg.getOwner();
  Value in;
  for (auto it = b->pred_begin(), e = b->pred_end(); it != e; ++it) {
    auto br = dyn_cast<BranchOpInterface>((*it)->getTerminator());
    if (!br)
      return {};
    Value v = br.getSuccessorOperands(it.getSuccessorIndex())
                  .getForwardedOperands()[arg.getArgNumber()];
    if (in && v != in)
      return {};
    in = v;
  }
  return in;
}

Value CondWriter::singleIncoming(BlockArgument arg) {
  return singleIncomingValue(arg);
}

std::string CondWriter::render(Value v, unsigned depth) {
  if (auto arg = dyn_cast<BlockArgument>(v)) {
    // following an argument counts as a level (bounds a cycle)
    Value in = depth < kConditionDepth ? singleIncoming(arg) : Value();
    return in ? render(in, depth + 1) : "?";
  }
  Operation *op = v.getDefiningOp();
  // leaves: input sites and constants, at any depth, within the node bound
  if (++nodes > kConditionNodes)
    return "?";
  if (auto n = dyn_cast<NondetOp>(op))
    return site(n.getSite());
  if (auto n = dyn_cast<StructNondetOp>(op))
    return site(n.getSite());
  if (auto c = dyn_cast<ConstOp>(op)) {
    if (auto a = dyn_cast<IntegerAttr>(c.getValue()))
      return llvm::toString(a.getValue(), 10, /*Signed=*/false);
    return "?";
  }
  if (auto s = dyn_cast<SegmentOp>(op))
    return llvm::toString(s.getBase(), 10, /*Signed=*/false);
  if (depth >= kConditionDepth)
    return "?";
  if (auto l = dyn_cast<LoadOp>(op)) {
    // only a load from the initial memory of a sea.mem.nondet that nothing
    // but loads uses (an inplace update would change the cells it reads):
    // the cells it reads are that site's inputs
    auto m = l.getMem().getDefiningOp<MemNondetOp>();
    if (!m || llvm::any_of(m->getUsers(),
                           [](Operation *u) { return !isa<LoadOp>(u); }))
      return "?";
    // align is a promise; no value depends on it
    for (NamedAttribute a : op->getAttrs())
      if (a.getName() != "align")
        return "?";
    std::string w = std::to_string(l.getType().getIntOrFloatBitWidth());
    std::string s = site(m.getSite());
    return "(load:" + w + " " + s + "@" + render(l.getAddr(), depth + 1) + ")";
  }
  // An allow-list: the ops whose value the rendering fully shows, with the
  // attributes it shows (or that no value depends on). Anything else, and
  // an allowed op with an attribute not listed, is `?`.
  const llvm::StringSet<> *attrs = allowedAttrs(op);
  if (!attrs)
    return "?";
  for (NamedAttribute a : op->getAttrs())
    if (!attrs->contains(a.getName().strref()))
      return "?";
  std::string head;
  if (auto c = dyn_cast<ICmpOp>(op))
    head = stringifyICmpPredicate(c.getPredicate()).str();
  else if (auto c = dyn_cast<AddrCmpOp>(op))
    head = stringifyICmpPredicate(c.getPredicate()).str();
  else if (auto c = dyn_cast<PtrCmpOp>(op))
    head = stringifyICmpPredicate(c.getPredicate()).str();
  else
    head = op->getName().getStringRef().drop_front(4).str(); // "sea."
  // wrap flags (add, sub, mul, shl): promises, shown
  if (op->hasAttr("nsw"))
    head += ".nsw";
  if (op->hasAttr("nuw") && !isa<PtrIndexOp>(op))
    head += ".nuw";
  std::string text = "(" + head;
  // the result width of the ops that change it: casts, extract
  if (isa<ZExtOp, SExtOp, TruncOp, ExtractOp>(op))
    text += ":" +
            std::to_string(op->getResult(0).getType().getIntOrFloatBitWidth());
  if (auto g = dyn_cast<GlobalOp>(op))
    return text + " @" + g.getName().str() + ")";
  if (auto f = dyn_cast<FuncAddrOp>(op))
    return text + " @" + f.getName().str() + ")";
  for (Value o : op->getOperands())
    text += " " + render(o, depth + 1);
  if (auto e = dyn_cast<ExtractOp>(op))
    text += " " + std::to_string(e.getLow());
  if (auto e = dyn_cast<StructExtractOp>(op))
    text += " " + std::to_string(e.getIndex());
  if (auto e = dyn_cast<StructInsertOp>(op))
    text += " " + std::to_string(e.getIndex());
  if (auto pi = dyn_cast<PtrIndexOp>(op)) {
    text +=
        " strides=" +
        llvm::join(llvm::map_range(pi.getStrides(),
                                   [](int64_t x) { return std::to_string(x); }),
                   ",");
    text += " offset=" + std::to_string(pi.getOffset());
    if (pi.getInbounds())
      text += " inbounds";
    if (pi.getNusw())
      text += " nusw";
    if (pi.getNuw())
      text += " nuw";
  }
  return text + ")";
}

//===----------------------------------------------------------------------===//
// Ranges of value sites, and the export form's final assume
//===----------------------------------------------------------------------===//

/// `v`, through block arguments with one incoming value (at most
/// kConditionDepth of them, as describeCondition).
static Value strip(Value v) {
  for (unsigned i = 0; i < kConditionDepth; ++i) {
    auto arg = dyn_cast<BlockArgument>(v);
    if (!arg)
      return v;
    Value in = singleIncomingValue(arg);
    if (!in)
      return v;
    v = in;
  }
  return v;
}

/// The site of a sea.nondet of at most 64 bits.
static std::optional<uint64_t> rangeSite(Value v) {
  auto n = strip(v).getDefiningOp<NondetOp>();
  if (!n || n.getType().getIntOrFloatBitWidth() > 64)
    return std::nullopt;
  return n.getSite();
}

static std::optional<uint64_t> constOf(Value v) {
  auto c = strip(v).getDefiningOp<ConstOp>();
  auto a = c ? dyn_cast<IntegerAttr>(c.getValue()) : IntegerAttr();
  if (!a || a.getValue().getBitWidth() > 64)
    return std::nullopt;
  return a.getValue().getZExtValue();
}

/// `v` as a sum of constants and value sites (sea.add, any flags: the value
/// does not depend on them), within describeCondition's bounds.
static bool sumOf(Value v, unsigned depth, unsigned &nodes, uint64_t mask,
                  InputRange::Sum &out) {
  if (++nodes > kConditionNodes)
    return false;
  if (std::optional<uint64_t> c = constOf(v)) {
    out.constant = (out.constant + *c) & mask;
    return true;
  }
  if (std::optional<uint64_t> s = rangeSite(v)) {
    out.sites.push_back(*s);
    return true;
  }
  auto add = strip(v).getDefiningOp<AddOp>();
  if (!add || depth >= kConditionDepth)
    return false;
  return sumOf(add.getLhs(), depth + 1, nodes, mask, out) &&
         sumOf(add.getRhs(), depth + 1, nodes, mask, out);
}

namespace {
/// One recognized range assume: site in lo + [0, span or bound].
struct RangeFact {
  uint64_t site = 0;
  unsigned width = 0;
  uint64_t lo = 0;
  std::optional<uint64_t> span;
  std::optional<InputRange::Sum> bound;
  bool empty = false; // (ult .. 0)
};
} // namespace

static std::optional<RangeFact> rangeFactOf(AssumeOp a) {
  auto cmp = strip(a.getCond()).getDefiningOp<ICmpOp>();
  if (!cmp)
    return std::nullopt;
  unsigned w = cmp.getLhs().getType().getIntOrFloatBitWidth();
  if (w > 64)
    return std::nullopt;
  uint64_t mask = w == 64 ? ~uint64_t(0) : (uint64_t(1) << w) - 1;
  RangeFact f;
  f.width = w;
  ICmpPredicate pred = cmp.getPredicate();
  if (pred == ICmpPredicate::ult) {
    std::optional<uint64_t> k = constOf(cmp.getRhs());
    if (!k)
      return std::nullopt;
    if (std::optional<uint64_t> s = rangeSite(cmp.getLhs())) {
      f.site = *s;
    } else if (auto add = strip(cmp.getLhs()).getDefiningOp<AddOp>()) {
      // (ult (add sN M) K): sN - (-M) in [0, K-1]
      std::optional<uint64_t> s = rangeSite(add.getLhs());
      std::optional<uint64_t> m = constOf(add.getRhs());
      if (!s || !m) {
        s = rangeSite(add.getRhs());
        m = constOf(add.getLhs());
      }
      if (!s || !m)
        return std::nullopt;
      f.site = *s;
      f.lo = (0 - *m) & mask;
    } else {
      return std::nullopt;
    }
    if (*k == 0)
      f.empty = true;
    else
      f.span = *k - 1;
    return f;
  }
  if (pred == ICmpPredicate::ule) {
    std::optional<uint64_t> s = rangeSite(cmp.getLhs());
    if (!s)
      return std::nullopt;
    f.site = *s;
    InputRange::Sum e;
    unsigned nodes = 0;
    if (!sumOf(cmp.getRhs(), 0, nodes, mask, e))
      return std::nullopt;
    if (e.sites.empty())
      f.span = e.constant;
    else
      f.bound = std::move(e);
    return f;
  }
  return std::nullopt;
}

std::vector<InputRange> inputRangesOf(ProgramOp program, RangeStats &stats) {
  stats = RangeStats();
  std::map<uint64_t, InputRange> bySite;
  for (auto [id, op] : llvm::enumerate(blockingOpsOf(program))) {
    auto a = dyn_cast<AssumeOp>(op);
    if (!a)
      continue;
    std::optional<RangeFact> f = rangeFactOf(a);
    if (!f)
      continue;
    if (f->empty) {
      ++stats.skipped;
      continue;
    }
    auto [it, fresh] = bySite.try_emplace(f->site);
    InputRange &r = it->second;
    if (fresh) {
      r.site = f->site;
      r.width = f->width;
      r.lo = f->lo;
    } else if (r.lo != f->lo) {
      ++stats.skipped; // another offset: intervals do not intersect simply
      continue;
    }
    if (f->span)
      r.spanConst = r.spanConst ? std::min(*r.spanConst, *f->span) : *f->span;
    if (f->bound)
      r.bounds.push_back(std::move(*f->bound));
    r.assumes.push_back(id);
  }
  // drop the ranges whose bounds reach back to themselves (cycles)
  std::map<uint64_t, int> state; // 0 new, 1 on the stack, 2 done
  std::set<uint64_t> cyclic;
  std::function<bool(uint64_t)> visit = [&](uint64_t s) {
    int &st = state[s];
    if (st == 1)
      return false;
    if (st == 2)
      return !cyclic.count(s);
    st = 1;
    bool ok = true;
    if (auto it = bySite.find(s); it != bySite.end())
      for (const InputRange::Sum &b : it->second.bounds)
        for (uint64_t t : b.sites)
          ok = visit(t) && ok;
    state[s] = 2;
    if (!ok)
      cyclic.insert(s);
    return ok;
  };
  std::vector<InputRange> out;
  for (auto &[s, r] : bySite) {
    if (!visit(s)) {
      stats.skipped += r.assumes.size();
      continue;
    }
    ++stats.sites;
    stats.assumes += r.assumes.size();
    out.push_back(std::move(r));
  }
  return out;
}

bool isAssumeBeforeAssertFalse(Operation *op) {
  if (!isa_and_nonnull<AssumeOp>(op))
    return false;
  llvm::SmallPtrSet<Block *, 4> seen;
  Operation *next = op->getNextNode();
  while (next) {
    if (auto br = dyn_cast<BrOp>(next)) {
      Block *dest = br.getDest();
      if (!seen.insert(dest).second || dest->empty())
        return false;
      next = &dest->front();
      continue;
    }
    auto as = dyn_cast<AssertOp>(next);
    if (!as) {
      // pure ops (the constant of the assert) change nothing on the way
      if (!isPure(next))
        return false;
      next = next->getNextNode();
      continue;
    }
    std::optional<uint64_t> c = constOf(as.getCond());
    return c && *c == 0;
  }
  return false;
}

ConditionText describeCondition(Value cond) {
  CondWriter w;
  ConditionText out;
  out.text = w.render(cond, 0);
  out.sites.assign(w.sites.begin(), w.sites.end());
  return out;
}

} // namespace sea
