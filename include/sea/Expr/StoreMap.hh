// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Store-Map support (SeaM, thesis Ch. 6 and Ch. 7.2; docs/design/SeaM.md,
// "Store-Map representation").
//
// A store-map term store-map(arr, base, ocl) (ExprOpArray.hh) is the array
// `arr` with the cells base + o_k written with v_k, for the (offset, value)
// entries of `ocl`, an insertion-ordered cons list (thesis Fig. 7.2) of
// smap-cons(offset, value, rest) ending in smap-nil. The offsets are
// numerals, so two entries with different offsets are different addresses
// and the order of the writes does not matter: the term means the same with
// no assumption. An offset may occur several times in the list (an insert
// puts the new entry in front, it does not remove the old one); the first
// one met is the latest, the others are shadowed.
//
// Two things live here.
//
//  StoreMapTable: the ordered maps (offset -> latest value) that make a
//  lookup logarithmic. Expr is immutable, so the map cannot be a part of the
//  term: it is a side table owned by the rewriter (thesis Ch. 7.2: a cache
//  from store-maps to ordered maps, moved to the new key on an insert).
//  Differences from the thesis and the SeaHorn branch:
//   - the key is the list node, not the store-map node. The map is a
//     function of the list alone, so a store-map rebuilt over another
//     array (the commute rule WOSMAP-COMMUTE, or a rewritten kid) keeps its
//     map with no transfer hook (the branch needed onAfterRewrite for it).
//   - the keys are owning Exprs (no manual Ref / Deref, which the branch got
//     wrong: a store-map already in the cache was counted twice).
//   - an insert into a list whose map was moved away earlier (a version of
//     the memory that was extended twice: a join, or an old memory read
//     again) rebuilds the map from the list (counted, smap.rebuild) so that
//     the new version has one; a lookup in such a list scans it linearly
//     (counted, smap.scan; the thesis: "the offset-value ordered map would not
//     be available, a linear search of the OCL").
//   - the list is never compacted (the thesis does not): shadowed entries stay.
//
//  expandStoreMaps: store-map is not an SMT-LIB term. Before a term goes to a
//  solver (or its model is asked for a value) every store-map in it must
//  become a chain of stores. Store-maps live in the
//  memory values of the VC generator (which is the point of the data
//  structure: the next store updates one), and every read over one is
//  rewritten away (R-HIT, R-MISS, R-SKIP, R-ABORT: no select over a store-map
//  survives a rewrite, unless --seam-row-expand=0 leaves a select over a store
//  that the compare cannot decide: that one is final, its store-map reaches
//  the boundary), so none reaches the query in practice. The
//  boundary that makes it a guarantee and not a hope is SeaOpSem::solverTerm
//  (the query and the model evaluation go through it); it counts what it
//  expanded (smap.expanded), which is 0 unless a rewrite was aborted
//  (maxRuleCalls), a memory term is used as a value, or --seam-row-expand=0.
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprRewriteRules.hh"
#include "sea/Expr/ExprRewriter.hh"

#include "llvm/ADT/SmallVector.h"

#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace expr {

/// The value of an offset numeral reduced mod 2^w (bv::bvnum does not reduce).
inline mpz_class smapOffsetKey(const Expr &numeral) {
  // 2^w per width, computed once (a lookup and every list walk ask for it
  // for each node)
  static thread_local std::unordered_map<unsigned, mpz_class> mods;
  unsigned w = op::bv::widthBvNum(numeral);
  auto it = mods.find(w);
  if (it == mods.end()) {
    mpz_class mod;
    mpz_ui_pow_ui(mod.get_mpz_t(), 2, w);
    it = mods.emplace(w, std::move(mod)).first;
  }
  mpz_class r;
  mpz_fdiv_r(r.get_mpz_t(), op::bv::toMpz(numeral).get_mpz_t(),
             it->second.get_mpz_t());
  return r;
}

/// One entry of an offset-value list.
struct SMapEntry {
  Expr offset; // a bit-vector numeral
  Expr value;
};

/// The distinct offsets of the list \p ocl with their latest values, oldest
/// insertion first (the order a store chain would write them). O(length).
inline void smapEntries(const Expr &ocl, std::vector<SMapEntry> &out) {
  std::set<mpz_class> seen;
  std::vector<SMapEntry> newestFirst;
  for (Expr cur = ocl; isOpX<SMAP_CONS>(cur); cur = cur->arg(2))
    if (seen.insert(smapOffsetKey(cur->arg(0))).second)
      newestFirst.push_back({cur->arg(0), cur->arg(1)});
  out.assign(newestFirst.rbegin(), newestFirst.rend());
}

/// The ordered maps of the store-maps of one rewriter (see the header).
class StoreMapTable {
public:
  using Map = std::map<mpz_class, Expr>;

  explicit StoreMapTable(RewriteStats &stats) : m_stats(stats) {}

  /// The latest value written at \p off in \p ocl, or a null Expr: from the
  /// ordered map of the list when it has one (O(log n)), else by scanning the
  /// list (smap.scan, O(length)).
  Expr find(const Expr &ocl, const mpz_class &off) {
    m_stats.count("smap.lookup");
    auto it = m_maps.find(ocl);
    if (it != m_maps.end()) {
      auto e = it->second.find(off);
      return e == it->second.end() ? Expr() : e->second;
    }
    m_stats.count("smap.scan");
    for (Expr cur = ocl; isOpX<SMAP_CONS>(cur); cur = cur->arg(2))
      if (smapOffsetKey(cur->arg(0)) == off)
        return cur->arg(1);
    return Expr();
  }

  /// The list \p ocl with (offset, value) inserted in front (thesis Fig. 7.2):
  /// the new list takes over the ordered map of \p ocl (moved: \p ocl has
  /// none afterwards). If \p ocl has none (it was moved earlier) the map is
  /// rebuilt from the list first (smap.rebuild).
  Expr insert(const Expr &ocl, const Expr &offset, const Expr &value) {
    Expr nocl = op::array::smapCons(offset, value, ocl);
    if (m_maps.count(nocl))
      return nocl; // the same insert again (terms are hash-consed)
    Map m;
    auto it = m_maps.find(ocl);
    if (it != m_maps.end()) {
      m = std::move(it->second);
      m_maps.erase(it);
      m_stats.count("smap.move");
    } else {
      m = build(ocl);
    }
    m[smapOffsetKey(offset)] = value;
    m_maps.emplace(nocl, std::move(m));
    return nocl;
  }

  /// A new list of two entries with its map (SMAP-NEW): (o1, v1) is the older
  /// entry, (o2, v2) the later one. The offsets must differ.
  Expr make2(const Expr &o1, const Expr &v1, const Expr &o2, const Expr &v2) {
    Expr ocl = op::array::smapCons(
        o2, v2, op::array::smapCons(o1, v1, op::array::smapNil(o1->efac())));
    if (!m_maps.count(ocl)) {
      Map m;
      m[smapOffsetKey(o1)] = v1;
      m[smapOffsetKey(o2)] = v2;
      m_maps.emplace(ocl, std::move(m));
    }
    return ocl;
  }

  /// The distinct entries of \p ocl, oldest first. Walks the list.
  static std::vector<SMapEntry> entries(const Expr &ocl) {
    std::vector<SMapEntry> v;
    smapEntries(ocl, v);
    return v;
  }

  size_t size() const { return m_maps.size(); }
  void clear() { m_maps.clear(); }

private:
  Map build(const Expr &ocl) {
    m_stats.count("smap.rebuild");
    Map m;
    for (const SMapEntry &e : entries(ocl))
      m[smapOffsetKey(e.offset)] = e.value;
    return m;
  }

  RewriteStats &m_stats;
  std::unordered_map<Expr, Map, ExprPtrHash> m_maps;
};

/// Rewrites every store-map of a term into a chain of stores (see the
/// header). One instance keeps a memo, so terms that share structure are
/// expanded once; destroy it before the ExprFactory of its terms. Iterative:
/// memory terms of a VC are deep.
class StoreMapExpander {
public:
  /// \p root with every store-map replaced by the stores of its entries, oldest
  /// first, over the expansion of its array. A term with no store-map is
  /// returned as it is.
  Expr expand(const Expr &root) {
    std::vector<std::pair<Expr, bool>> stack;
    stack.push_back({root, false});
    while (!stack.empty()) {
      Expr e = stack.back().first;
      bool kidsDone = stack.back().second;
      stack.pop_back();
      if (m_memo.count(e))
        continue;
      if (rules::isLeaf(e)) {
        m_memo.emplace(e, e);
        continue;
      }
      bool isMap = isOpX<STORE_MAP>(e);
      if (!kidsDone) {
        stack.push_back({e, true});
        if (isMap) {
          push(stack, e->arg(0));
          push(stack, e->arg(1));
          for (const SMapEntry &en : StoreMapTable::entries(e->arg(2)))
            push(stack, en.value);
        } else {
          for (size_t i = 0; i < e->arity(); ++i)
            push(stack, e->arg(i));
        }
        continue;
      }
      if (isMap) {
        ++m_expanded;
        Expr base = m_memo.find(e->arg(1))->second;
        Expr res = m_memo.find(e->arg(0))->second;
        for (const SMapEntry &en : StoreMapTable::entries(e->arg(2))) {
          Expr addr = smapOffsetKey(en.offset) == 0
                          ? base
                          : op::ptr::addressPlus(base, en.offset);
          res = op::array::store(res, addr, m_memo.find(en.value)->second);
        }
        m_memo.emplace(e, res);
        continue;
      }
      llvm::SmallVector<Expr, 4> kids;
      bool changed = false;
      for (size_t i = 0; i < e->arity(); ++i) {
        const Expr &k = m_memo.find(e->arg(i))->second;
        changed = changed || k.get() != e->arg(i);
        kids.push_back(k);
      }
      m_memo.emplace(
          e, changed ? e->getFactory().mkNary(e->op(), kids.begin(), kids.end())
                     : e);
    }
    return m_memo.find(root)->second;
  }

  /// store-map nodes expanded so far (0: none was in the terms)
  uint64_t expanded() const { return m_expanded; }

private:
  void push(std::vector<std::pair<Expr, bool>> &stack, const Expr &k) {
    if (!m_memo.count(k))
      stack.push_back({k, false});
  }

  std::unordered_map<Expr, Expr, ExprPtrHash> m_memo;
  uint64_t m_expanded = 0;
};

} // end of namespace expr
