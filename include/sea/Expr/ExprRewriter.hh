// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Multi-step Expr rewriter (SeaM, thesis Ch. 7.1).
//
// Ported from the SeaHorn branch seam-dev14 (include/seahorn/Expr/
// ExprRewriter.hh). Uses a Config template (shouldRewrite /
// doRewrite / onAfterRewrite / shouldCache), the statuses RW_DONE / RW_<n> /
// RW_FULL / RW_SKIP, an iterative post-order traversal over a frame stack
// and a result stack, and the nested-rewrite ("multi-step") caching of
// thesis Alg. 4-5.
//
// Nested-rewrite caching. A rule can produce a term whose rewriting it
// enables (a ~> b ~> c). Plain per-node caching stores only b |-> c, so the
// next visit of `a` runs the rule again and re-walks the b part. In a DAG
// where `a` is reached from many parents (thesis Fig. 7.1) that turns the
// DAG into a tree, and makes repeated visits exponential. The rewriter marks
// the frame that started a chain and caches start |-> final when the chain
// ends (RW_DONE / RW_SKIP). Config switches in RewriteOptions disable it
// (tests only) so the test can show that it is what keeps the work linear.
//
// The cache is depth aware: each entry records how many levels below its
// term were rewritten, and a visit needing more levels cannot reuse it. This
// prevents a bounded rewrite from standing in for a full rewrite.
// The frame stack, result stack, nested-rewrite cache, and RewriteStats are
// owned by the rewriter; RewriteOptions controls test-only traversal variants.
//
// Differences from z3 (src/ast/rewriter/rewriter_def.h, rewriter.h):
//  - z3, in process_app, keeps the frame of `t` in state REWRITE_BUILTIN
//    while the follow-up term is rewritten, then caches t |-> final. That is
//    the same chain-start caching, but z3 does it for every hop of a chain
//    (each follow-up frame is again an application frame) and only for
//    terms accepted by must_cache: ref_count > 1, not the root, not a
//    constant. Here the first frame of a chain and the last one are cached
//    (the intermediate terms of a chain are not), and the policy is
//    Config::shouldCache. Whether z3's policy would have sufficed for SeaM
//    is not known; the tests measure the variants (RewriteOptions).
//  - z3 visits the follow-up term through visit(), so a follow-up that is
//    already in the cache costs nothing. The branch pushes the follow-up
//    frame directly, which re-runs the rules at the follow-up root. Default
//    here is the branch's; see followUpLookup.
//  - Config::shouldCache is also consulted for the node's own use_count,
//    which here includes the references held by the rewriter's own frames
//    (z3's frames hold raw pointers). In practice almost every node
//    that has a parent counts as shared; the always-cache hook
//    (utils::shouldCache: select / store / ite) is kept from the branch.
//  - z3 caches per scope and with proofs; neither is needed here.
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprCore.hh"
#include "sea/Expr/RewriteTrail.hh"
#include "sea/Support/Diag.h"
#include "sea/Support/Stats.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace expr {

namespace utils {
/// \brief Per-operator always-cache hook (SeaHorn seam-dev14
/// utils::shouldCache): select, store and ite are cached whatever their
/// sharing, so that rebuilds of the same term (hash-consed) are found
/// again; others only when shared.
inline bool shouldCache(const Expr &e) {
  if (isOpX<SELECT>(e) || isOpX<STORE>(e) || isOpX<ITE>(e)) {
    return true; // aggressive cache row and wow
  }
  return e->use_count() > 1;
}
} // end of namespace utils

/// \brief What a rule says about the term it returns.
///
/// RW_DONE: simplification is final. RW_1 .. RW_3: rewrite the result again,
/// the rules are applied at the result and n levels below it (RW_1: at the
/// result and its kids; the kids' kids are taken as they are). RW_FULL:
/// rewrite the result without a bound.
/// RW_SKIP: the result needs no further visit (the rules do not apply to
/// it); only return it for terms shouldRewrite() rejects.
enum RWStatus {
  RW_DONE = 0,
  RW_1 = 1,
  RW_2 = 2,
  RW_3 = 3,
  RW_FULL = 4,
  RW_SKIP = 5
};

/// Depth meaning "no bound" (the frame depth of RW_FULL).
constexpr unsigned kFullDepth = std::numeric_limits<unsigned>::max();

/// Frame depth of the follow-up of a rule that returned \p s (Alg. 2 line 25:
/// RW_n gives depth n).
inline unsigned depthOf(RWStatus s) {
  return s == RWStatus::RW_FULL ? kFullDepth : static_cast<unsigned>(s);
}

struct RewriteResult {
  Expr exp;
  RWStatus status;
};

struct RewriteFrame {
  // the Expr node (subtree) under rewrite
  Expr m_exp;

  // number of levels to rewrite below this node
  unsigned m_depth;
  // up to m_i th children have been rewritten
  size_t m_i;
  // this frame started a multi-step rewrite and is waiting for its end
  // (a marker: it is never processed)
  bool m_rewriting;

  RewriteFrame(Expr exp, unsigned depth, size_t i, bool rw = false)
      : m_exp(std::move(exp)), m_depth(depth), m_i(i), m_rewriting(rw) {}
};

using RewriteFrameVector = std::vector<RewriteFrame>;

/// \brief Hash of an Expr by node identity (terms are hash-consed).
struct ExprPtrHash {
  size_t operator()(const Expr &e) const {
    return std::hash<const void *>()(e.get());
  }
};

/// \brief Cache from a term to its rewritten form, with the depth of the
/// rewrite.
///
/// An entry records its cover: the number of levels below the term that were
/// rewritten (kFullDepth: all). A lookup that needs a cover accepts an entry
/// with at least that cover; a bounded rewrite is never taken for a full one.
///
/// Keys and values are owning Exprs: a cached node stays alive (and its
/// address stays its identity) as long as it is in the cache. The cache must
/// be destroyed (or cleared) before the ExprFactory of its terms. A cache may
/// be reused by several rewriteExpr calls with the same rules.
class RewriteCache {
  struct Entry {
    Expr rw;
    unsigned cover;
  };
  std::unordered_map<Expr, Entry, ExprPtrHash> m_map;

public:
  /// \brief the rewritten form of \p e with a cover of at least \p cover, or
  /// nullptr
  const Expr *find(const Expr &e, unsigned cover) const {
    auto it = m_map.find(e);
    return (it == m_map.end() || it->second.cover < cover) ? nullptr
                                                           : &it->second.rw;
  }
  /// \brief true if the cache changed: a new entry, or an entry replaced by
  /// one of a larger cover
  bool insert(const Expr &src, const Expr &rw, unsigned cover) {
    auto res = m_map.emplace(src, Entry{rw, cover});
    if (res.second)
      return true;
    if (res.first->second.cover >= cover)
      return false;
    res.first->second = Entry{rw, cover};
    return true;
  }
  size_t size() const { return m_map.size(); }
  void clear() { m_map.clear(); }
};

/// \brief Counters of the rewriter. Events are counted by the traversal,
/// per-rule counters by the rules (count()).
struct RewriteStats {
  /// doRewrite calls (a frame whose kids are done)
  uint64_t ruleCalls = 0;
  /// doRewrite calls that changed the term or asked for a further rewrite
  /// ("rule applications")
  uint64_t rulesFired = 0;
  /// lookups that found a cached result
  uint64_t cacheHits = 0;
  /// entries added to the cache
  uint64_t cacheInserts = 0;
  /// follow-up terms requested by RW_n / RW_FULL
  uint64_t chainHops = 0;
  /// multi-step rewrites marked (Alg. 4)
  uint64_t chainStarts = 0;
  /// chain start |-> final added to the cache (Alg. 5)
  uint64_t chainStartCaches = 0;
  /// per-rule application counts, named by the rules
  llvm::StringMap<uint64_t> rules;
  /// the rule trail (RewriteTrail.hh); null: off, nothing recorded
  RewriteTrail *trail = nullptr;

  void count(llvm::StringRef rule) {
    ++rules[rule];
    if (trail)
      trail->note(rule);
  }
  /// a rule's use of premises (seam::Premise bits), for the trail
  void notePremise(unsigned p) {
    if (trail)
      trail->notePremise(p);
  }
  /// adds the rule counts of \p o (a scratch set of counters whose rules
  /// are kept), noting them in the trail, names in sorted order
  void merge(const RewriteStats &o) {
    std::vector<std::pair<std::string, uint64_t>> v;
    for (auto &kv : o.rules)
      v.emplace_back(kv.getKey().str(), kv.getValue());
    std::sort(v.begin(), v.end());
    for (auto &kv : v) {
      rules[kv.first] += kv.second;
      if (trail)
        trail->note(kv.first);
    }
  }
  uint64_t get(llvm::StringRef rule) const {
    auto it = rules.find(rule);
    return it == rules.end() ? 0 : it->second;
  }
  void reset() {
    RewriteTrail *t = trail;
    *this = RewriteStats();
    trail = t;
  }

  /// \brief `<prefix><counter>: <n>` lines, rules (`<prefix>rule.<name>`)
  /// sorted by name
  void report(sea::stats::Report &r, llvm::StringRef prefix = "") const {
    auto line = [&](const llvm::Twine &k) -> sea::stats::Line & {
      return r.line((prefix + k).str());
    };
    line("rule-calls").value(ruleCalls);
    line("rules-fired").value(rulesFired);
    line("cache-hits").value(cacheHits);
    line("cache-inserts").value(cacheInserts);
    line("chain-hops").value(chainHops);
    line("chain-starts").value(chainStarts);
    line("chain-start-caches").value(chainStartCaches);
    std::vector<std::pair<std::string, uint64_t>> v;
    for (auto &kv : rules)
      v.emplace_back(kv.getKey().str(), kv.getValue());
    std::sort(v.begin(), v.end());
    for (auto &kv : v)
      line("rule." + kv.first).value(kv.second);
  }
};

/// \brief Switches of the traversal. The defaults are the product
/// configuration; the others exist for the tests and measurements.
struct RewriteOptions {
  /// Nested-rewrite caching (thesis Alg. 4-5): cache the start of a
  /// multi-step rewrite. Off: the first design of the thesis (Alg. 2 only;
  /// a ~> b ~> c caches b |-> c). Test only.
  bool chainStartCache = true;
  /// Visit the follow-up term of RW_n through the cache (z3's behavior)
  /// instead of pushing its frame directly (SeaHorn branch). Measurements.
  bool followUpLookup = false;
  /// Stop a rewriteExpr call after this many rule calls (0 = no limit): the
  /// call returns its root unchanged and aborted() is set; the cache keeps
  /// what was found. Tests: a regression fails instead of running for hours.
  uint64_t maxRuleCalls = 0;
};

/// \brief Base class for rewrite configuration
///
/// A Config derives from it and provides
///   bool shouldRewrite(const Expr &e)   rules may apply to e
///   RewriteResult doRewrite(const Expr &e)   apply them (kids are done)
/// and may hide shouldCache / onAfterRewrite.
class ExprRewriterConfigBase {
protected:
  ExprFactory &m_efac;
  // per-rule counters (the rules count into it)
  RewriteStats &m_stats;

public:
  ExprRewriterConfigBase(ExprFactory &efac, RewriteStats &stats)
      : m_efac(efac), m_stats(stats) {}

  bool shouldCache(const Expr &e) const { return e->use_count() > 1; }

  /// Event handler for after successful rewrite
  void onAfterRewrite(const Expr &oldE, const Expr &newE) const {}
};

/// \brief A generic bottom up expression rewriter
///
/// Rewrite rules are embedded in \p Config template argument
template <typename Config>
class ExprRewriter {
protected:
  Config &m_config;
  RewriteCache &m_cache;
  RewriteStats &m_stats;
  RewriteOptions m_opts;

  RewriteFrameVector m_rewriteStack;
  ExprVector m_resultStack;
  bool m_aborted = false;

  /*** \brief visits an expression

       returns true if expression has been rewritten, and false if it is placed
       back on the rewrite stack

    visit \p e, return true if any of the following is true:
    1. e has been cached, or
    2. current depth is 0 or
    3. current \p m_config  dictates that \p e should not be further visited
    in any of the above cases, push \p e to top of \p m_resultStack
    otherwise return false and push \p e into top of \p m_rewriteStack
  */
  bool visit(const Expr &e, unsigned depth) {
    // -- no need to rewrite
    if (depth == 0 || !m_config.shouldRewrite(e)) {
      m_resultStack.push_back(e);
      return true;
    }

    // -- lookup in cache (the policy bounds what is inserted; a probe of a
    // node that is not there costs one hash lookup). The frame of e would
    // have depth nextDepth: the entry must cover as much.
    unsigned nextDepth = (depth == kFullDepth) ? depth : depth - 1;
    if (const Expr *cached = m_cache.find(e, nextDepth)) {
      ++m_stats.cacheHits;
      m_resultStack.push_back(*cached);
      return true;
    }

    // -- place new item on rewrite stack
    m_rewriteStack.emplace_back(e, nextDepth, 0);
    return false;
  }

  /// \brief adds the pair (src, rw), rewritten \p cover levels deep, to the
  /// cache
  bool addToCache(const Expr &src, const Expr &rw, unsigned cover) {
    if (!m_config.shouldCache(src))
      return false;
    if (m_cache.insert(src, rw, cover)) {
      ++m_stats.cacheInserts;
      return true;
    }
    return false;
  }

  /// \brief Returns true if the rewrite is nested in a rewrite: the frame
  /// below the one being finished is a multi-step marker
  bool isInNestedRewrite() const {
    return !m_rewriteStack.empty() && m_rewriteStack.back().m_rewriting;
  }

  /// \brief Alg. 5: a frame ended with the final result \p res (already on
  /// the result stack). If it ended a multi-step rewrite, cache the start of
  /// it and remove its marker. The cover of the entry is the depth of the
  /// start frame: RW_n is the rule's promise that n levels of the terms it
  /// builds are enough (given normal kids), so a hop does not lower the
  /// cover of the start; a start visited under a bound of its parent has that
  /// bound. Returns true if it did.
  bool endMultiStep(const Expr &res) {
    if (!isInNestedRewrite())
      return false;
    if (addToCache(m_rewriteStack.back().m_exp, res,
                   m_rewriteStack.back().m_depth))
      ++m_stats.chainStartCaches;
    m_rewriteStack.pop_back();
    return true;
  }

public:
  ExprRewriter(Config &config, RewriteCache &cache, RewriteStats &stats,
               RewriteOptions opts = RewriteOptions())
      : m_config(config), m_cache(cache), m_stats(stats), m_opts(opts) {}

  RewriteOptions &options() { return m_opts; }

  /// \brief true if the last rewriteExpr stopped at maxRuleCalls
  bool aborted() const { return m_aborted; }

  void processTopFrame() {
    {
      RewriteFrame &top = m_rewriteStack.back();
      const size_t arity = top.m_exp->arity();
      while (top.m_i < arity) {
        Expr kid(top.m_exp->arg(top.m_i++));
        // visit may push a frame (invalidating top); top is not used after
        if (!visit(kid, top.m_depth))
          return;
      }
    }

    // -- the frame is copied before it is popped: it owns exp, which is used
    // below (the branch used a reference to the popped element)
    RewriteFrame frame = m_rewriteStack.back();
    m_rewriteStack.pop_back();
    const Expr &exp = frame.m_exp;
    const size_t arity = exp->arity();

    // all kids of exp has been visited, collect rewritten kids to form
    // new expression
    bool changed = false;
    llvm::SmallVector<Expr, 4> new_kids;

    size_t end = m_resultStack.size();
    size_t begin = end - arity;

    for (size_t i = begin, j = 0; i < end; i++, j++) {
      const Expr &kid = m_resultStack[i];
      new_kids.push_back(kid);
      changed = changed || (kid.get() != exp->arg(j));
    }

    Expr newExp = changed ? exp->getFactory().mkNary(
                                exp->op(), new_kids.begin(), new_kids.end())
                          : exp;
    m_resultStack.resize(begin);

    // apply rewrite rules to expression with new kids
    ++m_stats.ruleCalls;
    // the trail: the rules counted in this call make one firing
    // newExp ~> rwRes.exp (a cache hit never gets here: counted only)
    RewriteTrail *trail = m_stats.trail;
    if (trail)
      trail->begin();
    RewriteResult rwRes = m_config.doRewrite(newExp);
    if (trail)
      trail->end(newExp, rwRes.exp);
    if (rwRes.exp != newExp || (rwRes.status != RWStatus::RW_DONE &&
                                rwRes.status != RWStatus::RW_SKIP))
      ++m_stats.rulesFired;
    m_config.onAfterRewrite(exp, rwRes.exp);

    switch (rwRes.status) {
    case RWStatus::RW_SKIP:
      m_resultStack.push_back(rwRes.exp);

      // cache nested rewrite
      endMultiStep(rwRes.exp);
      break;
    case RWStatus::RW_DONE:
      m_resultStack.push_back(rwRes.exp);

      // cache nested rewrite and the term of this frame (Alg. 2 lines
      // 20-21). In a chain the intermediate terms are not cached.
      addToCache(exp, rwRes.exp, frame.m_depth);
      endMultiStep(rwRes.exp);
      break;
    default: {
      ++m_stats.chainHops;
      /* current frame is the base of a multi-step rewrite, keep in rw stack
       * (Alg. 4) */
      if (!isInNestedRewrite() && m_opts.chainStartCache) {
        frame.m_rewriting = true;
        m_rewriteStack.push_back(frame);
        ++m_stats.chainStarts;
      }
      // Alg. 2 line 25: RW_n gives the follow-up frame depth n
      unsigned depth = depthOf(rwRes.status);
      if (m_opts.followUpLookup) {
        // z3: the follow-up term is visited like any other (visit takes the
        // depth of the visit, one more than the depth of its frame)
        unsigned visitDepth = depth == kFullDepth ? depth : depth + 1;
        if (visit(rwRes.exp, visitDepth))
          endMultiStep(m_resultStack.back());
      } else {
        // branch: the follow-up frame is pushed directly
        m_rewriteStack.emplace_back(rwRes.exp, depth, 0);
      }
    }
    }
  }

  Expr rewriteExpr(Expr root) {
    assert(m_rewriteStack.empty() && m_resultStack.empty());
    m_aborted = false;
    uint64_t callsAtStart = m_stats.ruleCalls;
    if (!visit(root, kFullDepth)) {
      while (!m_rewriteStack.empty()) {
        if (m_opts.maxRuleCalls &&
            m_stats.ruleCalls - callsAtStart >= m_opts.maxRuleCalls) {
          m_aborted = true;
          sea::diag::warn(
              "rewrite.abort",
              "rewriter stopped at maxRuleCalls; term not rewritten");
          m_rewriteStack.clear();
          m_resultStack.clear();
          return root;
        }
        processTopFrame();
      }
    }
    assert(m_resultStack.size() == 1);
    Expr res = m_resultStack.back();
    m_resultStack.pop_back();
    return res;
  }
};

} // end of namespace expr
