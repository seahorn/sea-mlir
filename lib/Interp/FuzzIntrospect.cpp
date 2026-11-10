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
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

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

Value CondWriter::singleIncoming(BlockArgument arg) {
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

ConditionText describeCondition(Value cond) {
  CondWriter w;
  ConditionText out;
  out.text = w.render(cond, 0);
  out.sites.assign(w.sites.begin(), w.sites.end());
  return out;
}

} // namespace sea
