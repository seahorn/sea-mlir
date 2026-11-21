// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// The cells plan (docs/design/Ackermann.md, "Cell representatives: the
// plan"): a dry run that says, per group of objects of a lowered array,
// whether reading them through their cells would cost less than the
// Ackermann constraints the lowering keeps for their reads. It changes
// nothing: the input is a lowering made only for the plan (Ackermannizer
// with recordPairs and keepImages), whose query is discarded.
//
// Objects come from ARM (AddrRangeMap.hh), not from the index's structure:
// the caller gives, per read, the join of the ARM maps of the access
// addresses behind it (AccessOrigins), or nothing (no recorded access, a
// shadow read, more than kMaxAddrs addresses). A read is
//  - unknown: no map, a top map, an absolute entry (a loaded or numeric
//    address: no provenance), an address whose ARM provenance no A-INB
//    condition names (Rules::armInbGap), an entry of an object of size
//    >= 2^63, or no base of nonzero size;
//  - known: on the bases of its map, each with an interval of offsets
//    (arm-interval: every interval bounded; full-object: some interval top,
//    then all the object's cells, resting on A-INB).
// Bases read together by one read form one group (a read on two objects is
// a case split over the cells of both); a group is converted as a whole.
//
// Candidates of a known read on base b of size N: the offsets in
// [0, N) ∩ I_b; for a read on one base whose index splits as b + rest (the
// offset before narrowing for a read narrowed in b), also ∩ the unsigned
// interval of rest (BvRanges) and the residue of rest's linear form
// (c mod 2^t, t the trailing zeros common to its coefficients); the
// array's cells only (an aligned word memory of k-unit cells: offsets
// 0 mod k when b's alignment is a multiple of k; else every unit); and,
// for at most kDeciderFilter candidates, minus those the difference
// decider finds never equal to rest. Exact given A-INB of the read's
// access (its offset is in [0, N) when it executes).
//
// Per group G (R = its reads, U = the array's unknown reads, C = the
// distinct candidate cells of R, arms = the sum of the candidates over R):
//  - keep = the constrained pairs with both reads in R or one in R and one
//    in U; the pairs with a read of another group are counted apart (a cell
//    mode drops them by A-SEP / A-OWN);
//  - count cost = arms + C + C x |U| (the rule as first stated);
//  - bit cost: keep x (aw + vw) against arms x vw (the mux) + the decoding,
//    C x vw (the cells) + C x |U| x (aw + vw), aw / vw the index / value
//    widths; the decoding is n x iw per distinct symbolic part of the
//    reads' offsets (rest's linear form; the index's for a read without
//    rest: the selectors of S + c_1 and S + c_2 test S, shared, also by the
//    arrays reading at S; counted at its first read), nothing for a read
//    with at most one candidate, iw = ceil(log2 C);
//  - converted: keep > 0 and cost < keep, by each measure (ratio 1,
//    untuned).
// Counts only, to decide whether the lowering is worth building.
#include "sea/Expr/Ackermann.hh"
#include "sea/Expr/AddrRangeMap.hh"
#include "sea/Expr/BvRange.hh"
#include "sea/Expr/DiffDecider.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/LinearForms.hh"
#include "sea/Support/Stats.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace expr {

/// An object of the plan, by ObjectBase::order: its base as the query has
/// it (erased), its size in address units (0: no cells), whether the plan can
/// use it (size < 2^63; a read touching one it cannot is unknown), its
/// alignment (the word-cell rule needs a k-aligned base), its name (the
/// log).
struct CellsObjectInfo {
  Expr base;
  uint64_t size = 0;
  bool usable = true;
  uint64_t align = 1;
  std::string name;
};

struct CellsPlan {
  /// at most this many cells per object are tracked one by one (C is exact
  /// below it; above, the sum of the reads' candidates, cellsBound)
  static constexpr uint64_t kMaxBitmap = uint64_t(1) << 24;
  /// a read with at most this many candidates is filtered by the difference
  /// decider (a cost bound on the plan, untuned; not a gate)
  static constexpr uint64_t kDeciderFilter = 64;

  struct Group {
    size_t array = 0;
    std::vector<unsigned> objects; ///< ObjectBase::order, increasing
    uint64_t reads = 0, readsInterval = 0, readsFull = 0;
    uint64_t unknown = 0; ///< |U|
    /// distinct symbolic parts of the reads' offsets decoded here (the
    /// selectors of offsets S + c_1, S + c_2 test S: shared)
    uint64_t indices = 0;
    uint64_t arms = 0, cells = 0;
    bool cellsBound = false;
    unsigned aw = 0, vw = 0, iw = 0;
    uint64_t keep = 0, other = 0;
    uint64_t cost = 0, keepBits = 0, costBits = 0;
    bool convCount = false, convBits = false;
  };
  std::vector<Group> groups;
  /// reads of the lowered arrays by source; of the known ones, those on more
  /// than one base and those whose candidates rest on A-INB (an interval not
  /// inside the object)
  uint64_t reads = 0, readsInterval = 0, readsFull = 0, readsUnknown = 0,
           readsMulti = 0, readsInb = 0;
  /// of the unknown reads: those with an access address whose ARM
  /// provenance no A-INB condition names (seam::Rules::armInbGap; set by
  /// the caller), and those touching an object the plan cannot use
  uint64_t readsInbGap = 0, readsUnusable = 0;
  /// bit writes into the cell bitmaps, overlaps included (the plan's own
  /// cost: one pass per
  /// distinct candidate progression of a group and base), groups whose C is
  /// a bound (an object over kMaxBitmap units)
  uint64_t cellsMarked = 0, groupsCellsBound = 0;
  /// constrained pairs of the lowering; of them those ARM separates (the
  /// maps of both reads disjoint): by different bases (A-SEP), by intervals
  /// of one base (exact)
  uint64_t pairs = 0, armCrossBase = 0, armInterval = 0;
  /// constrained pairs of two groups, one of them converted (count, bits)
  uint64_t otherCount = 0, otherBits = 0;
  /// candidates the difference decider dropped (kDeciderFilter)
  uint64_t deciderDropped = 0;

  void report(sea::stats::Report &r, llvm::StringRef prefix) const {
    auto line = [&](const llvm::Twine &k) -> sea::stats::Line & {
      return r.line((prefix + k).str());
    };
    uint64_t keep = 0, cells = 0, arms = 0, unk = 0;
    uint64_t cConv = 0, cReads = 0, cRemoved = 0, cAdded = 0;
    uint64_t bConv = 0, bReads = 0, bRemoved = 0, bKeep = 0, bCost = 0;
    for (const Group &g : groups) {
      keep += g.keep;
      cells += g.cells;
      arms += g.arms;
      unk += g.cells * g.unknown;
      if (g.convCount) {
        ++cConv;
        cReads += g.reads;
        cRemoved += g.keep;
        cAdded += g.cost;
      }
      if (g.convBits) {
        ++bConv;
        bReads += g.reads;
        bRemoved += g.keep;
        bKeep += g.keepBits;
        bCost += g.costBits;
      }
    }
    line("reads").value(reads);
    line("reads-arm-interval").value(readsInterval);
    line("reads-full-object").value(readsFull);
    line("reads-unknown").value(readsUnknown);
    line("reads-multi-base").value(readsMulti);
    line("reads-a-inb").value(readsInb);
    line("reads-inb-gap").value(readsInbGap);
    line("reads-unusable").value(readsUnusable);
    line("groups").value(groups.size());
    line("cells").value(cells);
    line("cells-marked").value(cellsMarked);
    line("groups-cells-bound").value(groupsCellsBound);
    line("arms").value(arms);
    line("decider-dropped").value(deciderDropped);
    line("unknown-constraints").value(unk);
    line("keep").value(keep);
    line("pairs").value(pairs);
    line("arm-pairs-cross-base").value(armCrossBase);
    line("arm-pairs-interval").value(armInterval);
    line("converted").value(cConv);
    line("reads-converted").value(cReads);
    line("removed").value(cRemoved);
    line("added").value(cAdded);
    line("other-object-pairs").value(otherCount);
    line("bits-converted").value(bConv);
    line("bits-reads-converted").value(bReads);
    line("bits-removed").value(bRemoved);
    line("bits-keep").value(bKeep);
    line("bits-cost").value(bCost);
    line("bits-other-object-pairs").value(otherBits);
  }
};

namespace cells_plan_detail {
/// Offsets x in [lo, hi] with x = f (mod 2^t) (t >= 64: x = f).
inline uint64_t progression(uint64_t lo, uint64_t hi, uint64_t f, unsigned t,
                            uint64_t &first) {
  if (lo > hi)
    return 0;
  if (t >= 64) {
    first = f;
    return f >= lo && f <= hi ? 1 : 0;
  }
  const uint64_t step = uint64_t(1) << t, mask = step - 1;
  // the least x >= lo with x = f (mod step)
  uint64_t x = (lo & ~mask) | (f & mask);
  if (x < lo) {
    if (x > UINT64_MAX - step)
      return 0;
    x += step;
  }
  if (x > hi)
    return 0;
  first = x;
  return (hi - x) / step + 1;
}

inline unsigned ceilLog2(uint64_t n) {
  unsigned b = 0;
  while (b < 64 && (uint64_t(1) << b) < n)
    ++b;
  return std::max(1u, b);
}
} // namespace cells_plan_detail

/// The plan over \p ack, a lowering made with recordPairs and keepImages.
/// \p readMap(a, k): the ARM map of read k of lowered array a (nullopt: no
/// recorded access). \p objs: by ObjectBase::order. \p cellUnits(a): the
/// cells of array a are at offsets 0 mod this (a power of two: the word of
/// an aligned word memory, whose layout pass proved it; 1 otherwise).
inline CellsPlan planCells(
    const Ackermannizer &ack,
    const std::function<std::optional<AddrRangeMap>(size_t, size_t)> &readMap,
    const std::vector<CellsObjectInfo> &objs,
    const std::function<unsigned(size_t)> &cellUnits) {
  using namespace cells_plan_detail;
  CellsPlan plan;
  const auto &arrs = ack.arrays();
  auto known = [&](unsigned key) {
    return key != AddrRangeMap::kAbs && key < objs.size() &&
           objs[key].size != 0;
  };
  auto unusable = [&](unsigned key) {
    return key == AddrRangeMap::kAbs || key >= objs.size() ||
           (objs[key].size != 0 && !objs[key].usable);
  };
  // per array, per read: its map (or none) and its group (-1: unknown)
  std::vector<std::vector<std::optional<AddrRangeMap>>> maps(arrs.size());
  std::vector<std::vector<long>> groupOf(arrs.size());
  // the decoded offsets: (width, symbolic part of rest's linear form), or
  // (0, the index's symbolic part Read::group) for a read without rest
  std::set<std::pair<unsigned, ackermann_detail::LinearForms::Key>> decoded;
  std::map<unsigned, std::unique_ptr<DiffDecider>> deciders;
  std::map<unsigned, std::unique_ptr<ackermann_detail::LinearForms>> forms;
  BvRanges ranges;
  for (size_t a = 0; a < arrs.size(); ++a) {
    const auto &rs = arrs[a].reads;
    const Expr sort = op::bind::declaredSort(arrs[a].array);
    const unsigned w = op::bv::width(sort::arrayIndexTy(sort));
    const unsigned vw = op::bv::width(sort::arrayValTy(sort));
    auto &lf = forms[w];
    if (!lf)
      lf = std::make_unique<ackermann_detail::LinearForms>(w);
    maps[a].resize(rs.size());
    groupOf[a].assign(rs.size(), -1);
    plan.reads += rs.size();
    // the known reads' bases; union-find over the bases read together
    std::vector<std::vector<unsigned>> bases(rs.size());
    std::unordered_map<unsigned, unsigned> parent;
    std::function<unsigned(unsigned)> find = [&](unsigned x) {
      unsigned p = parent.at(x);
      if (p == x)
        return x;
      return parent[x] = find(p);
    };
    uint64_t unknown = 0;
    for (size_t k = 0; k < rs.size(); ++k) {
      maps[a][k] = readMap(a, k);
      const auto &m = maps[a][k];
      bool ok = m && !m->isTop() && !m->hasAbs();
      if (ok)
        for (const AddrRangeMap::Entry &e : m->entries()) {
          if (unusable(e.first)) {
            ok = false;
            ++plan.readsUnusable;
            break;
          }
          if (known(e.first))
            bases[k].push_back(e.first);
        }
      if (!ok || bases[k].empty()) {
        bases[k].clear();
        ++unknown;
        continue;
      }
      for (unsigned b : bases[k])
        parent.emplace(b, b);
      for (unsigned b : bases[k])
        parent[find(b)] = find(bases[k][0]);
    }
    plan.readsUnknown += unknown;
    // the groups of this array
    std::map<unsigned, size_t> groupNo; // root -> plan.groups index
    for (size_t k = 0; k < rs.size(); ++k) {
      if (bases[k].empty())
        continue;
      unsigned root = find(bases[k][0]);
      auto [it, fresh] = groupNo.emplace(root, plan.groups.size());
      if (fresh) {
        CellsPlan::Group g;
        g.array = a;
        g.unknown = unknown;
        g.aw = w;
        g.vw = vw;
        plan.groups.push_back(g);
      }
      groupOf[a][k] = long(it->second);
    }
    for (auto &[b, p] : parent)
      plan.groups[groupNo.at(find(b))].objects.push_back(b);
    for (const auto &[root, gi] : groupNo)
      std::sort(plan.groups[gi].objects.begin(), plan.groups[gi].objects.end());
    // per group and base: the cells (a bitmap; empty above kMaxBitmap), and
    // the progressions marked in it (first, t, count: marked once)
    std::map<std::pair<size_t, unsigned>, std::vector<bool>> cellMap;
    std::set<std::tuple<size_t, unsigned, uint64_t, unsigned, uint64_t>> marked;
    // per group: (candidates, decoding key) of each read
    std::vector<std::vector<std::pair<
        uint64_t, std::pair<unsigned, ackermann_detail::LinearForms::Key>>>>
        perRead(plan.groups.size());
    for (size_t k = 0; k < rs.size(); ++k) {
      if (groupOf[a][k] < 0)
        continue;
      CellsPlan::Group &g = plan.groups[groupOf[a][k]];
      const Ackermannizer::Read &rd = rs[k];
      ++g.reads;
      bool full = false, inb = false;
      uint64_t n = 0;
      std::pair<unsigned, ackermann_detail::LinearForms::Key> dkey{
          0, ackermann_detail::LinearForms::Key{
                 {nullptr, mpz_class((unsigned long)rd.group)}}};
      if (bases[k].size() > 1)
        ++plan.readsMulti;
      for (unsigned b : bases[k]) {
        const CellsObjectInfo &o = objs[b];
        const AddrRange *r = maps[a][k]->find(b);
        // [lo, hi] = [0, N) ∩ I
        uint64_t lo = 0, hi = o.size - 1;
        if (!r || !r->isRange()) {
          full = true;
          inb = true;
        } else {
          if (r->lo().isNegative() || r->hi().sge(o.size))
            inb = true;
          if (r->hi().isNegative())
            continue;
          if (!r->lo().isNegative())
            lo = std::max(lo, r->lo().getZExtValue());
          if (r->hi().getActiveBits() <= 64)
            hi = std::min(hi, r->hi().getZExtValue());
        }
        // one base: the index's offset, its range and residue
        uint64_t f = 0;
        unsigned t = 0;
        // a narrowed read's offset before narrowing, if its base is b
        Expr rest = rd.rest && rd.nbase == o.base ? rd.rest : Expr();
        if (bases[k].size() == 1 &&
            (rest || Ackermannizer::splitBase(rd.index, o.base, w, rest))) {
          BvInterval ri = ranges.unsignedRange(rest);
          if (ri.known) {
            if (mpz_cmp_ui(ri.lo.get_mpz_t(), lo) > 0)
              lo = mpz_fits_ulong_p(ri.lo.get_mpz_t())
                       ? mpz_get_ui(ri.lo.get_mpz_t())
                       : hi + 1;
            if (mpz_cmp_ui(ri.hi.get_mpz_t(), hi) < 0)
              hi = mpz_get_ui(ri.hi.get_mpz_t());
          }
          const ackermann_detail::Linear &lin = lf->of(rest.get());
          dkey = {w, lf->key(lin)};
          t = w;
          for (const auto &[atom, coef] : lin.coef)
            t = std::min(t, Ackermannizer::trailingZeros(coef, w));
          if (t < 64) {
            mpz_class step(1UL), c;
            mpz_mul_2exp(step.get_mpz_t(), step.get_mpz_t(), t);
            mpz_fdiv_r(c.get_mpz_t(), lin.c.get_mpz_t(), step.get_mpz_t());
            f = mpz_get_ui(c.get_mpz_t());
          } else {
            if (!mpz_fits_ulong_p(lin.c.get_mpz_t()))
              continue;
            f = mpz_get_ui(lin.c.get_mpz_t());
          }
        }
        // the array's cells: offsets 0 mod u (u = 2^tu)
        const unsigned u0 = cellUnits(a);
        const unsigned u = o.align % u0 == 0 ? u0 : 1;
        unsigned tu = 0;
        while (tu < 63 && (uint64_t(1) << tu) < u)
          ++tu;
        bool none = false;
        if (t < tu) {
          none = (f & ((uint64_t(1) << t) - 1)) != 0;
          t = tu;
          f = 0;
        } else if (tu && (f & ((uint64_t(1) << tu) - 1)) != 0) {
          none = true;
        }
        uint64_t first = 0;
        uint64_t nb = none ? 0 : progression(lo, hi, f, t, first);
        const uint64_t step = t >= 64 ? 1 : uint64_t(1) << t;
        // a small set: drop the offsets the difference decider finds never
        // equal to rest (leaf by leaf through an ite offset; exact)
        std::vector<uint64_t> kept;
        const bool filter = rest && nb && nb <= CellsPlan::kDeciderFilter;
        if (filter) {
          auto &dd = deciders[w];
          if (!dd)
            dd = std::make_unique<DiffDecider>(w, nullptr);
          ExprFactory &efac = rest->efac();
          for (uint64_t i = 0, x = first; i < nb; ++i, x += step) {
            Expr num = op::bv::bvnum(mpz_class((unsigned long)x), w, efac);
            if (dd->decide(rest.get(), num.get()).verdict !=
                DiffDecider::Verdict::Never)
              kept.push_back(x);
          }
          plan.deciderDropped += nb - kept.size();
          nb = kept.size();
        }
        n += nb;
        if (o.size <= CellsPlan::kMaxBitmap && nb) {
          std::vector<bool> &bm = cellMap[{size_t(groupOf[a][k]), b}];
          if (bm.empty())
            bm.assign(o.size, false);
          if (filter) {
            for (uint64_t x : kept)
              bm[x] = true;
            plan.cellsMarked += nb;
          } else if (marked.insert({size_t(groupOf[a][k]), b, first, t, nb})
                         .second) {
            for (uint64_t i = 0, x = first; i < nb; ++i, x += step)
              bm[x] = true;
            plan.cellsMarked += nb;
          }
        } else if (nb) {
          if (!g.cellsBound)
            ++plan.groupsCellsBound;
          g.cellsBound = true;
          g.cells += nb; // a bound
        }
      }
      ++(full ? g.readsFull : g.readsInterval);
      ++(full ? plan.readsFull : plan.readsInterval);
      plan.readsInb += inb;
      g.arms += n;
      perRead[groupOf[a][k]].push_back({n, dkey});
    }
    for (const auto &[key, bm] : cellMap)
      plan.groups[key.first].cells +=
          uint64_t(std::count(bm.begin(), bm.end(), true));
    // the decoding, once per distinct symbolic part of the reads' offsets
    // (all arrays); nothing for a read with at most one candidate
    for (const auto &[root, gi] : groupNo) {
      CellsPlan::Group &g = plan.groups[gi];
      g.iw = ceilLog2(g.cells);
      uint64_t decode = 0;
      for (const auto &[n, key] : perRead[gi])
        if (n > 1 && decoded.insert(key).second) {
          ++g.indices;
          decode += n * g.iw;
        }
      g.costBits =
          g.arms * vw + decode + g.cells * vw + g.cells * g.unknown * (w + vw);
      g.cost = g.arms + g.cells + g.cells * g.unknown;
    }
  }
  // the constrained pairs: by group, and what ARM separates
  auto nonZero = [&](unsigned key) {
    return key < objs.size() && objs[key].size != 0;
  };
  std::vector<std::pair<long, long>> cross;
  for (const auto &p : ack.constrainedPairs()) {
    ++plan.pairs;
    const auto &mk = maps[p.array][p.k], &ml = maps[p.array][p.l];
    if (mk && ml) {
      bool usedCross = false;
      if (AddrRangeMap::disjoint(*mk, *ml, true, nonZero, &usedCross))
        ++(usedCross ? plan.armCrossBase : plan.armInterval);
    }
    long gk = groupOf[p.array][p.k], gl = groupOf[p.array][p.l];
    if (gk == gl) {
      if (gk >= 0)
        ++plan.groups[gk].keep;
      continue;
    }
    if (gk < 0 || gl < 0) {
      ++plan.groups[gk < 0 ? gl : gk].keep;
      continue;
    }
    ++plan.groups[gk].other;
    ++plan.groups[gl].other;
    cross.push_back({gk, gl});
  }
  for (CellsPlan::Group &g : plan.groups) {
    g.keepBits = g.keep * (g.aw + g.vw);
    g.convCount = g.keep > 0 && g.cost < g.keep;
    g.convBits = g.keep > 0 && g.costBits < g.keepBits;
  }
  for (const auto &[x, y] : cross) {
    plan.otherCount += plan.groups[x].convCount || plan.groups[y].convCount;
    plan.otherBits += plan.groups[x].convBits || plan.groups[y].convBits;
  }
  return plan;
}

} // namespace expr
