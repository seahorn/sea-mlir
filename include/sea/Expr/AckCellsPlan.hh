// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// The cells plan (docs/design/Ackermann.md, "Cell representatives: the
// plan"): a dry run that says, per (lowered array, known object), whether
// reading the object through its cells would cost fewer terms than the
// Ackermann constraints the lowering keeps for its reads. It changes nothing:
// the input is a lowering made only for the plan (Ackermannizer with
// recordPairs and setOwn), whose query is discarded.
//
// For an array A and an object o of size N (the own table's object of a read
// index; Read::obase):
//  - R = the reads of A in o, U = the reads of A in no known object;
//  - a read's candidates: its offset rest (index = o + rest at w bits; for a
//    narrowed read the offset before narrowing) as a linear form with
//    constant c and t = the trailing zeros common to its coefficients (w for
//    a numeral): the offsets off in [0, N) with off = c (mod 2^t), at most
//    maxEnum of them, minus those DiffDecider (no layout values: the base
//    cancels) finds never equal to rest; at most maxArms remain. Under A-INB
//    (the read's offset is in [0, N) when its load executes) the set holds
//    every offset the read can have;
//  - eligible: every read of R splits as o + rest and has a candidate set;
//  - keep = the constrained pairs of the lowering with both reads in R or
//    one in R and one in U (the pairs with a read of another known object
//    are not counted: a cell mode would drop them by A-OWN, own-pairs);
//  - cost = sum over R of its candidates (the arms of its case split) + C +
//    C x |U|, C = the distinct candidate offsets of R (one cell read each,
//    and one constraint per cell and unknown read);
//  - converted: eligible, keep > 0 and cost < keep (ratio 1, untuned).
// Counts only: what the conversion would remove (keep) against what it adds
// (cost), to decide whether the lowering is worth building.
#include "sea/Expr/Ackermann.hh"
#include "sea/Expr/DiffDecider.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/LinearForms.hh"
#include "sea/Support/Stats.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace expr {

/// The bounds of the candidate sets (first guesses, untuned).
struct CellsPlanOptions {
  /// at most this many offsets by the residue enumeration per read
  uint64_t maxEnum = 64;
  /// at most this many candidates per read after the decider's filter
  uint64_t maxArms = 16;
};

struct CellsPlan {
  /// Why an object is not eligible: a read whose index is not o + rest
  /// (shape), more than maxEnum offsets (enum) or maxArms candidates (arms).
  enum class Skip { None, Shape, Enum, Arms };
  struct Object {
    size_t array = 0;
    Expr base;        ///< the object constant (erased, as the own table)
    std::string name; ///< for the log
    uint64_t size = 0;
    uint64_t reads = 0;   ///< |R|
    uint64_t unknown = 0; ///< |U| (the array's reads in no known object)
    uint64_t arms = 0;    ///< sum of the candidates of R
    uint64_t cells = 0;   ///< C, the distinct candidate offsets
    uint64_t keep = 0;    ///< constrained pairs a cell mode removes
    uint64_t cost = 0;    ///< arms + C + C x |U|
    /// constrained pairs with a read of another known object (none if the
    /// lowering had the ownership drop on: then they are in ownDropped)
    uint64_t otherObject = 0;
    /// pairs the ownership drop dropped with a read in this object
    uint64_t ownDropped = 0;
    /// the largest residue enumeration of a read looked at (the log: how
    /// far over maxEnum a skipped object is)
    uint64_t enumMax = 0;
    /// the reads that failed: shape, over maxEnum, over maxArms
    uint64_t readsShape = 0, readsEnum = 0, readsArms = 0;
    /// the first read over maxEnum: the trailing zeros t of its offset's
    /// coefficients, its number of atoms, the atom with the fewest
    /// trailing zeros (the log: why the residues do not bound it)
    unsigned enumT = 0;
    uint64_t enumAtoms = 0;
    Expr enumAtom;
    Skip skip = Skip::None;
    bool converted = false;
  };
  /// in the order of the lowering's arrays, then of the objects' first reads
  std::vector<Object> objects;
  /// reads of the lowered arrays: in a known object, in none
  uint64_t reads = 0, ownReads = 0, unknownReads = 0;
  /// pairs of reads of two known objects, at least one converted: a cell
  /// mode drops them by A-OWN (the ownership drop of --ack-own, which may
  /// have dropped them already)
  uint64_t otherObjectPairs = 0;

  /// The `<prefix><counter>: <n>` lines (sea-vc `vc.cells-plan-`).
  void report(sea::stats::Report &r, llvm::StringRef prefix) const {
    auto line = [&](const llvm::Twine &k) -> sea::stats::Line & {
      return r.line((prefix + k).str());
    };
    uint64_t eligible = 0, skipShape = 0, skipEnum = 0, skipArms = 0,
             skipKeep = 0, skipCost = 0, converted = 0;
    uint64_t readsConv = 0, arms = 0, cells = 0, cost = 0, keep = 0,
             unknownCons = 0, eligKeep = 0, eligCost = 0, skippedKeep = 0;
    for (const Object &o : objects) {
      if (o.skip != Skip::None)
        skippedKeep += o.keep;
      switch (o.skip) {
      case Skip::Shape:
        ++skipShape;
        continue;
      case Skip::Enum:
        ++skipEnum;
        continue;
      case Skip::Arms:
        ++skipArms;
        continue;
      case Skip::None:
        break;
      }
      ++eligible;
      eligKeep += o.keep;
      eligCost += o.cost;
      if (!o.converted) {
        ++(o.keep == 0 ? skipKeep : skipCost);
        continue;
      }
      ++converted;
      readsConv += o.reads;
      arms += o.arms;
      cells += o.cells;
      cost += o.cost;
      keep += o.keep;
      unknownCons += o.cells * o.unknown;
    }
    line("objects").value(objects.size());
    line("eligible").value(eligible);
    line("skip-shape").value(skipShape);
    line("skip-enum").value(skipEnum);
    line("skip-arms").value(skipArms);
    line("skip-keep").value(skipKeep);
    line("skip-cost").value(skipCost);
    line("converted").value(converted);
    line("reads").value(reads);
    line("reads-own").value(ownReads);
    line("reads-unknown").value(unknownReads);
    line("reads-converted").value(readsConv);
    line("reads-kept").value(reads - readsConv);
    line("arms").value(arms);
    line("cells").value(cells);
    line("unknown-constraints").value(unknownCons);
    line("keep").value(eligKeep);
    line("keep-skipped").value(skippedKeep);
    line("cost").value(eligCost);
    line("removed").value(keep);
    line("added").value(cost);
    line("other-object-pairs").value(otherObjectPairs);
  }
};

namespace cells_plan_detail {
/// The candidate offsets of \p rest in an object of \p size units, at
/// width \p w (sorted): false if there are more than opt.maxEnum by the
/// residue enumeration (\p skip = Enum) or opt.maxArms after the filter
/// (\p skip = Arms).
inline bool candidates(const Expr &rest, uint64_t size, unsigned w,
                       ackermann_detail::LinearForms &lf, DiffDecider &dd,
                       const CellsPlanOptions &opt, std::vector<uint64_t> &out,
                       CellsPlan::Skip &skip, uint64_t &enumMax,
                       CellsPlan::Object *o = nullptr) {
  out.clear();
  const ackermann_detail::Linear &lin = lf.of(rest.get());
  unsigned t = w;
  ENode *low = nullptr;
  for (const auto &[atom, coef] : lin.coef) {
    unsigned z = Ackermannizer::trailingZeros(coef, w);
    if (z < t || !low)
      low = atom;
    t = std::min(t, z);
  }
  // offsets off in [0, size) with off = c (mod 2^t); size < 2^63
  mpz_class step(1UL), first;
  mpz_mul_2exp(step.get_mpz_t(), step.get_mpz_t(), t);
  mpz_fdiv_r(first.get_mpz_t(), lin.c.get_mpz_t(), step.get_mpz_t());
  if (mpz_cmp_ui(first.get_mpz_t(), size) >= 0)
    return true; // no offset in the object
  const uint64_t f = mpz_get_ui(first.get_mpz_t());
  const uint64_t count = t >= 63 ? 1 : (size - f + (uint64_t(1) << t) - 1) >> t;
  enumMax = std::max(enumMax, count);
  if (count > opt.maxEnum) {
    if (o && !o->enumAtom && low) {
      o->enumT = t;
      o->enumAtoms = lin.coef.size();
      o->enumAtom = Expr(low);
    }
    skip = CellsPlan::Skip::Enum;
    return false;
  }
  ExprFactory &efac = rest->efac();
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t off = f + (t >= 63 ? 0 : i << t);
    Expr num = op::bv::bvnum(mpz_class((unsigned long)off), w, efac);
    if (dd.decide(rest.get(), num.get()).verdict == DiffDecider::Verdict::Never)
      continue;
    out.push_back(off);
  }
  if (out.size() > opt.maxArms) {
    skip = CellsPlan::Skip::Arms;
    return false;
  }
  return true;
}
} // namespace cells_plan_detail

/// The plan over \p ack, a lowering made with recordPairs(true) and setOwn
/// (else every read is in no known object). \p sizes: object base (as in
/// the own table) -> size in address units and name; an object missing from
/// it is not eligible (shape).
struct CellsObjectInfo {
  uint64_t size = 0;
  std::string name;
};
inline CellsPlan
planCells(const Ackermannizer &ack,
          const std::unordered_map<ENode *, CellsObjectInfo> &info,
          CellsPlanOptions opt = CellsPlanOptions()) {
  using namespace cells_plan_detail;
  CellsPlan plan;
  const auto &arrs = ack.arrays();
  // per array: obase -> object number in plan.objects
  std::vector<std::unordered_map<ENode *, size_t>> objOf(arrs.size());
  std::map<unsigned, std::unique_ptr<ackermann_detail::LinearForms>> forms;
  std::map<unsigned, std::unique_ptr<DiffDecider>> deciders;
  for (size_t a = 0; a < arrs.size(); ++a) {
    const auto &rs = arrs[a].reads;
    unsigned w = op::bv::width(
        sort::arrayIndexTy(op::bind::declaredSort(arrs[a].array)));
    auto &lf = forms[w];
    if (!lf)
      lf = std::make_unique<ackermann_detail::LinearForms>(w);
    auto &dd = deciders[w];
    if (!dd)
      dd = std::make_unique<DiffDecider>(w, nullptr);
    uint64_t unknown = 0;
    for (const auto &rd : rs)
      unknown += rd.obase ? 0 : 1;
    plan.reads += rs.size();
    plan.unknownReads += unknown;
    plan.ownReads += rs.size() - unknown;
    // per object of this array: the distinct candidate offsets
    const size_t firstObj = plan.objects.size();
    std::vector<std::set<uint64_t>> cellSets;
    std::vector<uint64_t> cand;
    for (const auto &rd : rs) {
      if (!rd.obase)
        continue;
      auto [it, fresh] = objOf[a].emplace(rd.obase.get(), plan.objects.size());
      if (fresh) {
        CellsPlan::Object o;
        o.array = a;
        o.base = rd.obase;
        o.unknown = unknown;
        auto in = info.find(rd.obase.get());
        if (in == info.end()) {
          o.skip = CellsPlan::Skip::Shape;
        } else {
          o.size = in->second.size;
          o.name = in->second.name;
        }
        plan.objects.push_back(std::move(o));
        cellSets.emplace_back();
      }
      CellsPlan::Object &o = plan.objects[it->second];
      std::set<uint64_t> &cells = cellSets[it->second - firstObj];
      ++o.reads;
      if (o.size == 0)
        continue; // not in info
      // every read is looked at (the log counts the reads that fail); the
      // object takes the first failure
      CellsPlan::Skip why = CellsPlan::Skip::None;
      Expr rest = rd.rest;
      if (!rest && !Ackermannizer::splitBase(rd.index, rd.obase, w, rest))
        why = CellsPlan::Skip::Shape;
      else
        candidates(rest, o.size, w, *lf, *dd, opt, cand, why, o.enumMax, &o);
      if (why != CellsPlan::Skip::None) {
        ++(why == CellsPlan::Skip::Shape  ? o.readsShape
           : why == CellsPlan::Skip::Enum ? o.readsEnum
                                          : o.readsArms);
        if (o.skip == CellsPlan::Skip::None)
          o.skip = why;
        continue;
      }
      o.arms += cand.size();
      cells.insert(cand.begin(), cand.end());
    }
    for (size_t i = 0; i < cellSets.size(); ++i)
      plan.objects[firstObj + i].cells = cellSets[i].size();
  }
  // the read's object number in plan.objects (-1: in no known object)
  auto objNo = [&](size_t a, size_t k) -> long {
    const Expr &b = arrs[a].reads[k].obase;
    return b ? long(objOf[a].at(b.get())) : -1;
  };
  // the constrained pairs by object
  for (const auto &p : ack.constrainedPairs()) {
    long ok = objNo(p.array, p.k), ol = objNo(p.array, p.l);
    if (ok == ol) {
      if (ok >= 0)
        ++plan.objects[ok].keep;
      continue;
    }
    if (ok < 0 || ol < 0) {
      ++plan.objects[ok < 0 ? ol : ok].keep;
      continue;
    }
    ++plan.objects[ok].otherObject;
    ++plan.objects[ol].otherObject;
  }
  for (size_t a = 0; a < arrs.size(); ++a)
    ack.forEachOwnDropped(a, [&](size_t k, size_t l) {
      ++plan.objects[objNo(a, k)].ownDropped;
      ++plan.objects[objNo(a, l)].ownDropped;
    });
  for (CellsPlan::Object &o : plan.objects) {
    if (o.skip != CellsPlan::Skip::None)
      continue;
    o.cost = o.arms + o.cells + o.cells * o.unknown;
    o.converted = o.keep > 0 && o.cost < o.keep;
  }
  // the pairs of two known objects, one of them converted (each once)
  auto conv = [&](long o) { return o >= 0 && plan.objects[o].converted; };
  for (const auto &p : ack.constrainedPairs()) {
    long ok = objNo(p.array, p.k), ol = objNo(p.array, p.l);
    if (ok >= 0 && ol >= 0 && ok != ol && (conv(ok) || conv(ol)))
      ++plan.otherObjectPairs;
  }
  for (size_t a = 0; a < arrs.size(); ++a)
    ack.forEachOwnDropped(a, [&](size_t k, size_t l) {
      if (conv(objNo(a, k)) || conv(objNo(a, l)))
        ++plan.otherObjectPairs;
    });
  return plan;
}

} // namespace expr
