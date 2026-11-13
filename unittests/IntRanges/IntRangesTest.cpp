// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- IntRangesTest.cpp - the integer ranges of sea ops vs evalPureOp ----===//
//
// Plain-main driver (no gtest), an oracle (test/Transforms/int-ranges.test):
// the InferIntRangeInterface of every sea value op that has one
// (lib/Dialect/Sea/SeaIntRanges.cpp) is checked against the golden
// semantics (evalPureOp) at small widths: for every tuple of operand
// ranges from a set (every unsigned and every signed interval, and
// intersections of the two, which hold values in two pieces: all of them at
// widths 1-2, every 7th pair at width 3) and every
// tuple of values in them, the op's value is in its inferred range
// (soundness; precision is not checked). Ops with nsw / nuw: only the
// values that keep the promise (brokenWrapPromise) count. Also the pieces of
// sea-range-fold's facts (sea/Transforms/RangeFold.h): the bound of
// `icmp pred x, c` contains every x it holds for; swapPredicate; the step
// of a bound back through sext / zext contains every y whose extension is
// in the bound; tightenRange keeps the values of a range. Prints `checks: N`
// and `failures: N`; exit 1 on a failure.
//
//===----------------------------------------------------------------------===//

#include "sea/Dialect/Sea/SeaDialect.h"
#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Dialect/Sea/Semantics.h"
#include "sea/Transforms/RangeFold.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <string>
#include <tuple>
#include <vector>

using namespace mlir;
using llvm::APInt;

namespace {

uint64_t checks = 0, failures = 0;

using Key = std::tuple<uint64_t, uint64_t, int64_t, int64_t>;
Key keyOf(const ConstantIntRanges &r) {
  return {r.umin().getZExtValue(), r.umax().getZExtValue(),
          r.smin().getSExtValue(), r.smax().getSExtValue()};
}

bool contains(const ConstantIntRanges &r, const APInt &x) {
  return r.umin().ule(x) && x.ule(r.umax()) && r.smin().sle(x) &&
         x.sle(r.smax());
}

std::vector<APInt> members(const ConstantIntRanges &r) {
  std::vector<APInt> out;
  unsigned w = r.umin().getBitWidth();
  for (uint64_t v = 0; v < (uint64_t(1) << w); ++v)
    if (contains(r, APInt(w, v)))
      out.push_back(APInt(w, v));
  return out;
}

std::string str(const ConstantIntRanges &r) {
  std::string s;
  llvm::raw_string_ostream os(s);
  os << r;
  return s;
}

/// The operand ranges of width w: every unsigned and every signed interval;
/// for w <= `fullCross`, every non-empty intersection of an unsigned and a
/// signed one (else every `stride`-th).
std::vector<ConstantIntRanges> rangesOf(unsigned w) {
  std::map<Key, ConstantIntRanges> set;
  std::vector<ConstantIntRanges> u, s;
  uint64_t n = uint64_t(1) << w;
  for (uint64_t a = 0; a < n; ++a)
    for (uint64_t b = a; b < n; ++b) {
      u.push_back(ConstantIntRanges::fromUnsigned(APInt(w, a), APInt(w, b)));
      int64_t sa = APInt(w, a).getSExtValue();
      (void)sa;
    }
  int64_t lo = -(int64_t(1) << (w - 1)), hi = (int64_t(1) << (w - 1)) - 1;
  for (int64_t a = lo; a <= hi; ++a)
    for (int64_t b = a; b <= hi; ++b)
      s.push_back(
          ConstantIntRanges::fromSigned(APInt(w, a, true), APInt(w, b, true)));
  for (auto &r : u)
    set.try_emplace(keyOf(r), r);
  for (auto &r : s)
    set.try_emplace(keyOf(r), r);
  unsigned stride = w <= 2 ? 1 : 7, i = 0;
  for (auto &a : u)
    for (auto &b : s) {
      if (i++ % stride)
        continue;
      ConstantIntRanges r = a.intersection(b);
      if (!sea::isEmptyRange(r) && !members(r).empty())
        set.try_emplace(keyOf(r), r);
    }
  std::vector<ConstantIntRanges> out;
  for (auto &[k, r] : set)
    out.push_back(r);
  return out;
}

std::map<unsigned, std::vector<ConstantIntRanges>> rangeCache;
const std::vector<ConstantIntRanges> &ranges(unsigned w) {
  auto it = rangeCache.find(w);
  if (it == rangeCache.end())
    it = rangeCache.emplace(w, rangesOf(w)).first;
  return it->second;
}

/// Every op of the program with the interface, checked.
void checkOp(Operation *op) {
  auto iface = cast<InferIntRangeInterface>(op);
  unsigned k = op->getNumOperands();
  std::vector<unsigned> widths;
  for (Value o : op->getOperands())
    widths.push_back(sea::bitWidthOf(o.getType()));
  std::vector<size_t> idx(k, 0);
  while (true) {
    SmallVector<ConstantIntRanges> args;
    std::vector<std::vector<APInt>> vals;
    for (unsigned i = 0; i < k; ++i) {
      args.push_back(ranges(widths[i])[idx[i]]);
      vals.push_back(members(args.back()));
    }
    std::optional<ConstantIntRanges> res;
    iface.inferResultRanges(args, [&](Value v, const ConstantIntRanges &r) {
      if (v == op->getResult(0))
        res = r;
    });
    if (!res) {
      ++failures;
      llvm::errs() << "FAIL: no range for " << *op << "\n";
      return;
    }
    // every tuple of values
    std::vector<size_t> vi(k, 0);
    while (true) {
      SmallVector<APInt> ops;
      for (unsigned i = 0; i < k; ++i)
        ops.push_back(vals[i][vi[i]]);
      if (!sea::brokenWrapPromise(op, ops)) {
        std::optional<APInt> v = sea::evalPureOp(op, ops);
        ++checks;
        if (!v || !contains(*res, *v)) {
          if (++failures <= 20) {
            llvm::errs() << "FAIL: " << *op << " args";
            for (auto &a : args)
              llvm::errs() << " " << str(a);
            llvm::errs() << " values";
            for (auto &a : ops)
              llvm::errs() << " " << a.getZExtValue();
            llvm::errs() << " -> " << (v ? v->getZExtValue() : 999)
                         << " not in " << str(*res) << "\n";
          }
        }
      }
      unsigned i = 0;
      for (; i < k; ++i) {
        if (++vi[i] < vals[i].size())
          break;
        vi[i] = 0;
      }
      if (i == k)
        break;
    }
    unsigned i = 0;
    for (; i < k; ++i) {
      if (++idx[i] < ranges(widths[i]).size())
        break;
      idx[i] = 0;
    }
    if (i == k)
      break;
  }
}

std::string program(unsigned w) {
  std::string t = "i" + std::to_string(w);
  std::string s = "sea.program @w" + std::to_string(w) + " {\n^bb0:\n";
  s += "  %a = sea.nondet \"a\" [0] : " + t + "\n";
  s += "  %b = sea.nondet \"b\" [1] : " + t + "\n";
  s += "  %c = sea.nondet \"c\" [2] : i1\n";
  for (const char *op :
       {"add", "sub", "mul", "shl", "udiv", "sdiv", "urem", "srem", "and", "or",
        "xor", "lshr", "ashr", "umin", "umax", "smin", "smax"})
    s += std::string("  %") + op + " = sea." + op + " %a, %b : " + t + "\n";
  for (const char *op : {"add", "sub", "mul", "shl"})
    for (auto [tag, f] : {std::pair<const char *, const char *>{"s", "nsw"},
                          {"u", "nuw"},
                          {"su", "nsw nuw"}})
      s += std::string("  %") + op + "_" + tag + " = sea." + op + " %a, %b " +
           f + " : " + t + "\n";
  for (const char *p :
       {"eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge"})
    s += std::string("  %cmp_") + p + " = sea.icmp " + p + ", %a, %b : " + t +
         "\n";
  s += "  %sel = sea.select %c, %a, %b : " + t + "\n";
  s += "  sea.halt\n}\n";
  return s;
}

std::string castProgram() {
  std::string s = "sea.program @casts {\n^bb0:\n";
  for (unsigned w = 1; w <= 4; ++w)
    s += "  %a" + std::to_string(w) + " = sea.nondet \"a\" [" +
         std::to_string(w) + "] : i" + std::to_string(w) + "\n";
  int id = 0;
  for (unsigned n = 1; n <= 4; ++n)
    for (unsigned w = n + 1; w <= 4; ++w) {
      std::string from = "i" + std::to_string(n), to = "i" + std::to_string(w);
      for (const char *op : {"zext", "sext"})
        s += "  %x" + std::to_string(id++) + " = sea." + op + " %a" +
             std::to_string(n) + " : " + from + " to " + to + "\n";
      s += "  %x" + std::to_string(id++) + " = sea.trunc %a" +
           std::to_string(w) + " : " + to + " to " + from + "\n";
    }
  s += "  sea.halt\n}\n";
  return s;
}

bool holds(sea::ICmpPredicate p, const APInt &a, const APInt &b) {
  switch (p) {
  case sea::ICmpPredicate::eq:
    return a == b;
  case sea::ICmpPredicate::ne:
    return a != b;
  case sea::ICmpPredicate::slt:
    return a.slt(b);
  case sea::ICmpPredicate::sle:
    return a.sle(b);
  case sea::ICmpPredicate::sgt:
    return a.sgt(b);
  case sea::ICmpPredicate::sge:
    return a.sge(b);
  case sea::ICmpPredicate::ult:
    return a.ult(b);
  case sea::ICmpPredicate::ule:
    return a.ule(b);
  case sea::ICmpPredicate::ugt:
    return a.ugt(b);
  case sea::ICmpPredicate::uge:
    return a.uge(b);
  }
  return false;
}

void checkFacts() {
  const sea::ICmpPredicate preds[] = {
      sea::ICmpPredicate::eq,  sea::ICmpPredicate::ne,  sea::ICmpPredicate::slt,
      sea::ICmpPredicate::sle, sea::ICmpPredicate::sgt, sea::ICmpPredicate::sge,
      sea::ICmpPredicate::ult, sea::ICmpPredicate::ule, sea::ICmpPredicate::ugt,
      sea::ICmpPredicate::uge};
  for (unsigned w = 1; w <= 4; ++w)
    for (sea::ICmpPredicate p : preds)
      for (uint64_t c = 0; c < (uint64_t(1) << w); ++c) {
        APInt cv(w, c);
        std::optional<ConstantIntRanges> r = sea::boundOfPredicate(p, cv);
        if (!r && p != sea::ICmpPredicate::ne) {
          ++failures;
          llvm::errs() << "FAIL: no bound for a predicate other than ne\n";
        }
        for (uint64_t x = 0; x < (uint64_t(1) << w); ++x) {
          APInt xv(w, x);
          ++checks;
          if (holds(p, xv, cv) && r &&
              (sea::isEmptyRange(*r) || !contains(*r, xv))) {
            ++failures;
            llvm::errs() << "FAIL: bound of pred " << int(p) << " c=" << c
                         << " w=" << w << " misses " << x << "\n";
          }
          ++checks;
          if (holds(p, xv, cv) != holds(sea::swapPredicate(p), cv, xv)) {
            ++failures;
            llvm::errs() << "FAIL: swapPredicate " << int(p) << "\n";
          }
        }
      }
  // tightenRange keeps the values
  for (unsigned w = 1; w <= 4; ++w)
    for (const ConstantIntRanges &r : ranges(w)) {
      ConstantIntRanges t = sea::tightenRange(r);
      ++checks;
      if (members(t) != members(r)) {
        ++failures;
        llvm::errs() << "FAIL: tightenRange " << str(r) << " -> " << str(t)
                     << "\n";
      }
    }
  for (unsigned n = 1; n <= 3; ++n)
    for (unsigned w = n + 1; w <= 4; ++w)
      for (bool isSigned : {false, true})
        for (const ConstantIntRanges &r : ranges(w)) {
          ConstantIntRanges y = sea::rangeBeforeExt(r, isSigned, n);
          for (uint64_t v = 0; v < (uint64_t(1) << n); ++v) {
            APInt yv(n, v);
            APInt e = isSigned ? yv.sext(w) : yv.zext(w);
            ++checks;
            if (contains(r, e) && (sea::isEmptyRange(y) || !contains(y, yv))) {
              ++failures;
              llvm::errs() << "FAIL: rangeBeforeExt "
                           << (isSigned ? "sext" : "zext") << " i" << n
                           << " to i" << w << " of " << str(r) << " misses "
                           << v << " (got " << str(y) << ")\n";
            }
          }
        }
}

} // namespace

int main() {
  MLIRContext ctx;
  ctx.loadDialect<sea::SeaDialect>();
  std::vector<std::string> texts;
  for (unsigned w = 1; w <= 3; ++w)
    texts.push_back(program(w));
  texts.push_back(castProgram());
  for (const std::string &t : texts) {
    OwningOpRef<ModuleOp> m = parseSourceString<ModuleOp>(t, &ctx);
    if (!m) {
      llvm::errs() << "FAIL: parse\n" << t;
      return 1;
    }
    m->walk([&](Operation *op) {
      if (isa<InferIntRangeInterface>(op) && !isa<sea::ConstOp>(op))
        checkOp(op);
    });
  }
  // constants
  for (unsigned w = 1; w <= 4; ++w)
    for (uint64_t v = 0; v < (uint64_t(1) << w); ++v) {
      std::string t = "sea.program @k {\n^bb0:\n  %k = sea.const " +
                      std::to_string(v) + " : i" + std::to_string(w) +
                      "\n  sea.halt\n}\n";
      OwningOpRef<ModuleOp> m = parseSourceString<ModuleOp>(t, &ctx);
      m->walk([&](sea::ConstOp k) {
        std::optional<ConstantIntRanges> res;
        k.inferResultRanges(
            {}, [&](Value, const ConstantIntRanges &r) { res = r; });
        ++checks;
        if (!res || res->getConstantValue() != APInt(w, v)) {
          ++failures;
          llvm::errs() << "FAIL: const " << v << "\n";
        }
      });
    }
  checkFacts();
  llvm::outs() << "checks: " << checks << "\nfailures: " << failures << "\n";
  return failures ? 1 : 0;
}
