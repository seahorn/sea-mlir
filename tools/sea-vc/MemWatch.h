// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- MemWatch.h - what the VC terms grow by, while it grows ---*- C++ -*-===//
//
// sea-vc --mem-trace (= --log=vc.mem) and the budgets --max-nodes /
// --max-rss (docs/agents/blowups.md). An ExprFactory::Watch: the
// factory calls it every `step` nodes made, in the thread that makes them,
// so it sees the terms as they are and may read them (no other thread
// touches a factory). Each call checks the budgets and, every interval
// seconds, writes a beat: a `vc.mem beat` log event with the census, as it
// goes (a run killed later keeps the lines written so far).
//
// The census, read on demand:
//  - the phase (the last vc.phase start) and its run, the process memory
//    (rss-mb, vm-mb, peak-rss-mb), the live hash-consed nodes (the unique
//    table) and the nodes made so far;
//  - the operators with the most live nodes (a walk of the unique table:
//    linear in the live nodes; the next beat waits at least kCensusSpacing
//    times the walk's time, so the walk stays a small part of the run,
//    except with interval 0: a beat at every check);
//  - the rules (RewriteStats::rules of the sources: SeaM and the
//    simplifier) with the most firings since the last beat, and their
//    totals. A rule that runs nested rules on a scratch set of counters
//    (buildEq's ite push) adds them when it returns: a beat inside it sees
//    the counts before (never less than the last beat's; rule-calls
//    likewise);
//  - in encode, the sea ops that made the most nodes (the op in progress
//    included, with the nodes it made so far): SeaM rewrites a load or a
//    store as part of its op, so these are the reads and writes whose
//    rewriting grows the terms;
//  - the size of the rewriter's cache (its entries keep their terms alive).
// Nodes made outside encode (simplify, the solver boundary, Ackermann) are
// attributed to their phase only. The watch runs only when nodes are made:
// a phase that makes none (the solver's check) has its phase-end line only.
//
// Off (no flag): the factory has no watch, one compare per node made.
//
//===----------------------------------------------------------------------===//
#ifndef SEA_VC_TOOL_MEMWATCH_H
#define SEA_VC_TOOL_MEMWATCH_H

#include "sea/Expr/ExprCore.hh"
#include "sea/Expr/ExprRewriter.hh"
#include "sea/Interp/Interpreter.h"
#include "sea/Support/Log.h"
#include "sea/Support/ProcMem.h"
#include "sea/Support/Stats.h"
#include "sea/Support/Timer.h"

#include "mlir/IR/Operation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sea {

struct MemWatchOptions {
  /// seconds between beats (0: a beat at every check)
  double interval = 10;
  /// the length of each top list
  unsigned top = 8;
  /// budgets, 0: none: live hash-consed nodes; resident memory in MiB
  uint64_t maxNodes = 0;
  uint64_t maxRssMB = 0;
  /// the checks run every this many nodes made (a node budget lowers it)
  unsigned step = 4096;
};

class MemWatch : public expr::ExprFactory::Watch {
public:
  enum class Budget { Nodes, Rss };

  struct OpCount {
    std::string op;
    uint64_t live;
  };
  struct RuleCount {
    std::string rule;
    uint64_t total, recent;
  };
  struct SiteCount {
    std::string op, where;
    uint64_t nodes;
  };
  struct Census {
    std::string phase;
    unsigned run = 0;
    double time = 0;
    ProcMem mem;
    uint64_t nodesLive = 0, nodesMade = 0;
    /// rewriter cache entries; -1: no rewriter
    int64_t cacheEntries = -1;
    uint64_t ruleCalls = 0;
    std::vector<OpCount> ops;
    /// every rule fired so far, by recent firings
    std::vector<RuleCount> rules;
    std::vector<SiteCount> sites;
    /// the op being encoded (encode only), the nodes it made so far
    std::string curOp, curOpAt;
    uint64_t curOpNodes = 0;
  };

  /// \p onHit: a budget is exceeded, with the census of that moment; it
  /// must not return (sea-vc prints the stop and exits).
  using HitFn = std::function<void(Budget, uint64_t limit, const Census &)>;

  MemWatch(const log::Tag &tag, MemWatchOptions opts, HitFn onHit)
      : tag(tag), opts(opts), onHit(std::move(onHit)), t0(wallNow()),
        nextBeat(t0) {}

  /// The factory calls onNodes every step() nodes made: small enough that
  /// a node budget is checked within an eighth of it.
  unsigned step() const {
    const uint64_t s = opts.step;
    if (!opts.maxNodes)
      return unsigned(s);
    return unsigned(std::clamp<uint64_t>(opts.maxNodes / 8, 1, s));
  }

  /// The factory of a VC run and its rewriter (stats, cache; null: none),
  /// from its creation on; detach() before the factory goes.
  void attach(expr::ExprFactory &f, const expr::RewriteStats *stats,
              const expr::RewriteCache *cache) {
    fac = &f;
    rwStats = stats;
    rwCache = cache;
    lastRules.clear();
    lastRuleCalls = 0;
    sites.clear();
    curOp = nullptr;
    f.setWatch(this, step());
  }
  void detach() {
    if (fac)
      fac->setWatch(nullptr, 0);
    fac = nullptr;
    rwStats = nullptr;
    rwCache = nullptr;
    curOp = nullptr;
  }

  /// the rule counters of the attached run (null: none)
  const expr::RewriteStats *stats() const { return rwStats; }

  /// A phase starts (the vc.phase start event's name and run).
  void phaseStart(const char *name, unsigned run) {
    phase = name;
    phaseRun = run;
    if (name != std::string("encode"))
      curOp = nullptr;
    checkBudgets();
  }
  /// A phase ends: a `phase-end` line with the census (vc.mem on).
  void phaseEnd(const char *name) {
    checkBudgets();
    if (!tag.on(log::Flow))
      return;
    Census c = census();
    c.phase = name;
    emit("phase-end", c);
    lastBeatRulesSnapshot();
  }

  /// encode: \p op starts, \p made nodes made so far (null: no op, after a
  /// block)
  void opStart(mlir::Operation *op, uint64_t made) {
    if (curOp)
      sites[curOp] += made - curOpStart;
    curOp = op;
    curOpStart = made;
  }

  /// The budgets, now (also at each phase start and end: the memory a
  /// phase that makes no nodes took, the solver's, is seen at its end).
  void checkBudgets() {
    if (opts.maxNodes && fac && fac->nodesLive() > opts.maxNodes)
      onHit(Budget::Nodes, opts.maxNodes, census());
    if (opts.maxRssMB) {
      ProcMem m = readProcMem();
      if (m.hasCurrent && (m.rssKB >> 10) > opts.maxRssMB)
        onHit(Budget::Rss, opts.maxRssMB, census());
    }
  }

  void onNodes(expr::ExprFactory &) override {
    checkBudgets();
    if (!tag.on(log::Flow))
      return;
    const auto now = wallNow();
    if (now < nextBeat)
      return;
    Census c = census();
    emit("beat", c);
    lastBeatRulesSnapshot();
    const auto after = wallNow();
    // the next beat: interval seconds on, and at least kCensusSpacing
    // times the census's own time (interval 0: at every check, no spacing)
    if (opts.interval <= 0)
      return;
    const double cost = secondsBetween(now, after);
    nextBeat = after + std::chrono::duration_cast<SteadyClock::duration>(
                           std::chrono::duration<double>(std::max(
                               opts.interval, cost * kCensusSpacing)));
  }

  /// The census now (see the file comment).
  Census census() const {
    Census c;
    c.phase = phase.empty() ? "none" : phase;
    c.run = phaseRun;
    c.time = secondsBetween(t0, wallNow());
    c.mem = readProcMem();
    if (fac) {
      c.nodesLive = fac->nodesLive();
      c.nodesMade = fac->nodesMade();
      // the operators: grouped by their dynamic type (one per operator:
      // DefOp / Terminal instances), keyed by the vtable pointer (no RTTI
      // in this build; Itanium ABI: the object's first word), named once
      // per group
      llvm::DenseMap<const void *, std::pair<const expr::Operator *, uint64_t>>
          byType;
      fac->forEachLive([&](const expr::ENode *n) {
        const expr::Operator &op = n->op();
        auto &slot =
            byType[*reinterpret_cast<const void *const *>(&op)];
        if (!slot.first)
          slot.first = &op;
        ++slot.second;
      });
      llvm::StringMap<uint64_t> byName; // two types may share a name
      for (auto &kv : byType)
        byName[kv.second.first->name()] += kv.second.second;
      for (auto &kv : byName)
        c.ops.push_back({kv.getKey().str(), kv.getValue()});
      std::sort(c.ops.begin(), c.ops.end(), [](const auto &a, const auto &b) {
        return a.live != b.live ? a.live > b.live : a.op < b.op;
      });
      if (c.ops.size() > opts.top)
        c.ops.resize(opts.top);
    }
    if (rwCache)
      c.cacheEntries = int64_t(rwCache->size());
    if (rwStats) {
      c.ruleCalls = std::max(rwStats->ruleCalls, lastRuleCalls);
      for (auto &kv : rwStats->rules) {
        if (!kv.getValue())
          continue;
        auto it = lastRules.find(kv.getKey());
        const uint64_t last = it == lastRules.end() ? 0 : it->getValue();
        const uint64_t recent = kv.getValue() > last ? kv.getValue() - last : 0;
        c.rules.push_back({kv.getKey().str(), kv.getValue(), recent});
      }
      std::sort(c.rules.begin(), c.rules.end(),
                [](const auto &a, const auto &b) {
                  if (a.recent != b.recent)
                    return a.recent > b.recent;
                  if (a.total != b.total)
                    return a.total > b.total;
                  return a.rule < b.rule;
                });
      // all of them: a beat lists the top by recent firings, a budget stop
      // by total
    }
    // the sea ops of encode by nodes made, the one in progress included
    std::vector<std::pair<mlir::Operation *, uint64_t>> s(sites.begin(),
                                                          sites.end());
    if (curOp && fac) {
      const uint64_t partial = fac->nodesMade() - curOpStart;
      c.curOp = curOp->getName().getStringRef().str();
      c.curOpAt = where(curOp);
      c.curOpNodes = partial;
      auto it = std::find_if(s.begin(), s.end(),
                             [&](const auto &p) { return p.first == curOp; });
      if (it == s.end())
        s.push_back({curOp, partial});
      else
        it->second += partial;
    }
    // the largest first (a partial sort: the map can hold every op);
    // ties by location and op name (the map's order is by address)
    std::vector<SiteCount> all;
    all.reserve(s.size());
    for (auto &[op, n] : s)
      all.push_back({op->getName().getStringRef().str(), "", n});
    // locations are computed for the candidates only: those with at least
    // the top-th largest count
    std::vector<uint64_t> counts;
    for (auto &x : all)
      counts.push_back(x.nodes);
    uint64_t cut = 0;
    if (counts.size() > opts.top && opts.top) {
      std::nth_element(counts.begin(), counts.begin() + (opts.top - 1),
                       counts.end(), std::greater<uint64_t>());
      cut = counts[opts.top - 1];
    }
    std::vector<SiteCount> cand;
    for (size_t i = 0; i < all.size(); ++i)
      if (all[i].nodes >= cut && opts.top)
        cand.push_back({all[i].op, where(s[i].first), all[i].nodes});
    std::sort(cand.begin(), cand.end(), [](const auto &a, const auto &b) {
      if (a.nodes != b.nodes)
        return a.nodes > b.nodes;
      if (a.where != b.where)
        return a.where < b.where;
      return a.op < b.op;
    });
    if (cand.size() > opts.top)
      cand.resize(opts.top);
    c.sites = std::move(cand);
    return c;
  }

  /// `<item>,...`, `-` when empty (the beat's list fields)
  template <typename T, typename F>
  static std::string list(const std::vector<T> &v, F item) {
    std::string s;
    for (const T &x : v) {
      if (!s.empty())
        s += ',';
      s += item(x);
    }
    return s.empty() ? "-" : s;
  }

  /// the source position, else the location's name, else -
  static std::string where(mlir::Operation *op) {
    std::string t = "-";
    if (std::optional<mlir::FileLineColLoc> fl = fileLocOf(op->getLoc()))
      t = fileLocText(*fl);
    else if (std::optional<std::string> n = locNameOf(op->getLoc()))
      t = *n;
    return t;
  }

private:
  /// the census's walk may take at most 1 / kCensusSpacing of the run
  static constexpr double kCensusSpacing = 20;

  void emit(const char *msg, const Census &c) const {
    log::Event e(tag, log::Flow);
    e.msg(msg).kv("phase", c.phase);
    if (c.run)
      e.kv("run", c.run);
    e.kv("time", c.time);
    for (const auto &[k, v] : memFields(c.mem))
      e.kv(k, v);
    e.kv("nodes-live", c.nodesLive).kv("nodes-made", c.nodesMade);
    if (c.cacheEntries >= 0)
      e.kv("rw-cache", uint64_t(c.cacheEntries));
    if (rwStats)
      e.kv("rule-calls", c.ruleCalls);
    if (!c.curOp.empty())
      e.kv("op", c.curOp).kv("op-at", c.curOpAt).kv("op-nodes", c.curOpNodes);
    // list items count first (`<n>:<name>`): an operator name may hold a
    // colon (`:`, `mlir::Value`)
    e.kv("top-ops", list(c.ops, [](const OpCount &o) {
           return std::to_string(o.live) + ":" + o.op;
         }));
    if (rwStats) {
      std::vector<RuleCount> fired;
      for (const RuleCount &r : c.rules)
        if (r.recent && fired.size() < opts.top)
          fired.push_back(r);
      e.kv("top-rules", list(fired, [](const RuleCount &r) {
             return std::to_string(r.recent) + ":" + r.rule;
           }));
    }
    if (!c.sites.empty())
      e.kv("top-sites", list(c.sites, [](const SiteCount &s) {
             return std::to_string(s.nodes) + ":" + s.op + "@" + s.where;
           }));
  }

  /// the rule counts the next beat's recent counts start from: never
  /// lower than before (a nested scratch set of counters, file comment)
  void lastBeatRulesSnapshot() {
    if (!rwStats)
      return;
    lastRuleCalls = std::max(lastRuleCalls, rwStats->ruleCalls);
    for (auto &kv : rwStats->rules) {
      uint64_t &last = lastRules[kv.getKey()];
      last = std::max(last, kv.getValue());
    }
  }

  const log::Tag &tag;
  MemWatchOptions opts;
  HitFn onHit;
  SteadyClock::time_point t0, nextBeat;
  expr::ExprFactory *fac = nullptr;
  const expr::RewriteStats *rwStats = nullptr;
  const expr::RewriteCache *rwCache = nullptr;
  llvm::StringMap<uint64_t> lastRules;
  uint64_t lastRuleCalls = 0;
  std::string phase;
  unsigned phaseRun = 0;
  llvm::DenseMap<mlir::Operation *, uint64_t> sites;
  mlir::Operation *curOp = nullptr;
  uint64_t curOpStart = 0;
};

} // namespace sea

#endif // SEA_VC_TOOL_MEMWATCH_H
