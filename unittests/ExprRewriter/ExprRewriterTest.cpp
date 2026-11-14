// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ExprRewriterTest.cpp - tests of the multi-step Expr rewriter -------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if
// a check fails. Run by test/Expr/rewriter.test.
//
// The point of the tests is the nested-rewrite caching (thesis Ch. 7.1,
// Alg. 4-5, Fig. 7.1): a rewrite that enables a nested rewrite (a ~> b ~> c)
// must be cached at its start a, or a DAG that reaches `a` from several
// places is rewritten as a tree. The main test builds the thesis' Fig. 7.1
// scaled to N levels,
//
//   m_{k+1} = ite(c_k, store(m_k, p_k, v_k), store(m_k, q_k, w_k)),
//
// and reads select(m_N, r) with numeral addresses, so that every compare
// resolves and each read-over-write step is a nested rewrite chain reached
// from both ite arms. The checks are counts of rule applications (not
// time), so a regression fails deterministically.
//
// Every rewritten term is also checked against the original by evaluation
// under pseudo-random assignments of the conditions (a small evaluator of
// select / store / ite / = over numerals).
//
//===----------------------------------------------------------------------===//

// boost hash templates are only forward-declared by Expr.hh; define them here
// (a Release build does not export the instantiations from libSeaExpr).
#include "sea/Expr/ExprRewriter.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprRewriteRules.hh"
#include <boost/functional/hash.hpp>

#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace expr;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      llvm::errs() << "FAIL: " << msg << " [" #cond "] (line " << __LINE__     \
                   << ")\n";                                                   \
    }                                                                          \
  } while (0)

constexpr unsigned kWidth = 32;

/// A variant of the rewriter: the traversal switches and the caching policy.
struct Variant {
  const char *name;
  bool chainStartCache;
  bool followUpLookup;
  bool aggressiveCache;
};

// The product configuration, and the ablations measured by the table.
// (kDefault takes the traversal switches from RewriteOptions' defaults, so
// changing a default is caught by the tests of the default)
const Variant kDefault = {"default", RewriteOptions().chainStartCache,
                          RewriteOptions().followUpLookup, true};
const Variant kNoChain = {"no-chain-start", false, false, true};
const Variant kZ3Like = {"no-chain-start+followup-lookup", false, true, true};
const Variant kChainLookup = {"chain-start+followup-lookup", true, true, true};
const Variant kSharedOnly = {"shared-only-policy", true, false, false};
const Variant kNoChainShared = {"no-chain-start+shared-only", false, false,
                                false};

struct Builder {
  ExprFactory &efac;
  Expr bvSort, memSort;
  explicit Builder(ExprFactory &efac)
      : efac(efac), bvSort(bv::bvsort(kWidth, efac)),
        memSort(sort::arrayTy(bvSort, bvSort)) {}

  Expr num(unsigned long n) { return bv::bvnum(mpz_class(n), kWidth, efac); }
  Expr mem(const std::string &name) {
    return bind::mkConst(mkTerm<std::string>(name, efac), memSort);
  }
  Expr cond(const std::string &name) {
    return bind::boolConst(mkTerm<std::string>(name, efac));
  }
};

/// Evaluator of the terms of the tests under an assignment of the condition
/// constants. The base memory is the function i -> 7 i + 3 (mod 2^32).
class Evaluator {
  const std::map<ENode *, bool> &m_conds;
  Expr m_base;
  std::map<std::pair<ENode *, unsigned long>, unsigned long> m_memo;

public:
  Evaluator(const std::map<ENode *, bool> &conds, Expr base)
      : m_conds(conds), m_base(base) {}

  unsigned long scalar(const Expr &e) {
    if (bv::is_bvnum(e))
      return bv::toMpz(e).get_ui();
    if (isOpX<ITE>(e))
      return boolean(e->arg(0)) ? scalar(e->arg(1)) : scalar(e->arg(2));
    if (isOpX<SELECT>(e))
      return select(e->arg(0), scalar(e->arg(1)));
    llvm::errs() << "evaluator: unexpected scalar term\n";
    std::abort();
  }

  bool boolean(const Expr &e) {
    if (isOpX<TRUE>(e))
      return true;
    if (isOpX<FALSE>(e))
      return false;
    if (isOpX<EQ>(e))
      return scalar(e->arg(0)) == scalar(e->arg(1));
    auto it = m_conds.find(e.get());
    if (it != m_conds.end())
      return it->second;
    llvm::errs() << "evaluator: unexpected Boolean term\n";
    std::abort();
  }

  unsigned long select(const Expr &arr, unsigned long idx) {
    if (arr == m_base)
      return (7 * idx + 3) & 0xffffffffUL;
    auto key = std::make_pair(arr.get(), idx);
    auto it = m_memo.find(key);
    if (it != m_memo.end())
      return it->second;
    unsigned long res;
    if (isOpX<STORE>(arr))
      res = idx == scalar(arr->arg(1)) ? scalar(arr->arg(2))
                                       : select(arr->arg(0), idx);
    else if (isOpX<ITE>(arr))
      res = boolean(arr->arg(0)) ? select(arr->arg(1), idx)
                                 : select(arr->arg(2), idx);
    else {
      llvm::errs() << "evaluator: unexpected array term\n";
      std::abort();
    }
    m_memo[key] = res;
    return res;
  }
};

/// Distinct nodes of a term, and whether a store is among them.
struct Shape {
  size_t nodes = 0;
  bool hasStore = false;
};

Shape shapeOf(const Expr &root) {
  Shape s;
  std::set<ENode *> seen;
  std::vector<ENode *> todo{root.get()};
  while (!todo.empty()) {
    ENode *n = todo.back();
    todo.pop_back();
    if (!seen.insert(n).second)
      continue;
    ++s.nodes;
    Expr e(n);
    if (isOpX<STORE>(e))
      s.hasStore = true;
    for (size_t i = 0; i < n->arity(); ++i)
      todo.push_back(n->arg(i));
  }
  return s;
}

/// Whether \p before and \p after agree under several pseudo-random
/// assignments of \p conds (a deterministic LCG).
bool agree(const std::vector<Expr> &conds, const Expr &base, const Expr &before,
           const Expr &after) {
  unsigned long seed = 12345;
  for (int trial = 0; trial < 24; ++trial) {
    std::map<ENode *, bool> env;
    for (const Expr &c : conds) {
      seed = seed * 6364136223846793005UL + 1442695040888963407UL;
      env[c.get()] = (seed >> 33) & 1;
    }
    Evaluator ev(env, base);
    if (ev.scalar(before) != ev.scalar(after))
      return false;
  }
  return true;
}

struct Run {
  RewriteStats stats;
  bool aborted = false;
  bool sound = false;
  Shape shape;
};

RewriteOptions optionsOf(const Variant &v) {
  RewriteOptions o;
  o.chainStartCache = v.chainStartCache;
  o.followUpLookup = v.followUpLookup;
  return o;
}

enum class Scheme {
  /// store addresses k and 1000 + k, read at N / 2: the compare resolves
  /// everywhere, a hit only at one level on one arm
  Midpoint,
  /// addresses (7 k) mod 13 and (11 k + 3) mod 13, read at 5: hits on
  /// many levels on both arms
  Mixed
};

/// Fig. 7.1 scaled to \p n levels, rewrite select(m_n, r).
Run runFig71(unsigned n, Scheme scheme, const Variant &v,
             uint64_t maxRuleCalls = 0) {
  ExprFactory efac;
  Builder b(efac);
  Expr m0 = b.mem("m0");
  std::vector<Expr> conds;
  Expr m = m0;
  unsigned long r = scheme == Scheme::Midpoint ? n / 2 : 5;
  for (unsigned k = 0; k < n; ++k) {
    Expr c = b.cond("c" + std::to_string(k));
    conds.push_back(c);
    unsigned long p = scheme == Scheme::Midpoint ? k : (7 * k) % 13;
    unsigned long q = scheme == Scheme::Midpoint ? 1000 + k : (11 * k + 3) % 13;
    m = mk<ITE>(c, op::array::store(m, b.num(p), b.num(5000 + k)),
                op::array::store(m, b.num(q), b.num(9000 + k)));
  }
  Expr root = op::array::select(m, b.num(r));

  Run run;
  RewriteCache cache;
  MinimalRewriteConfig config(efac, run.stats, v.aggressiveCache);
  RewriteOptions opts = optionsOf(v);
  opts.maxRuleCalls = maxRuleCalls;
  ExprRewriter<MinimalRewriteConfig> rw(config, cache, run.stats, opts);
  Expr res = rw.rewriteExpr(root);
  run.aborted = rw.aborted();
  run.shape = shapeOf(res);
  // the evaluator is exponential-free (memoized), so it runs at any n
  run.sound = run.aborted || agree(conds, m0, root, res);
  return run;
}

void printRun(const char *test, const Variant &v, unsigned n, const Run &r) {
  llvm::outs() << test << " variant=" << v.name << " N=" << n
               << " rules-fired=" << r.stats.rulesFired
               << " rule-calls=" << r.stats.ruleCalls
               << " cache-hits=" << r.stats.cacheHits
               << " chain-starts=" << r.stats.chainStarts
               << " chain-start-caches=" << r.stats.chainStartCaches << "\n";
}

// ---- measured constants -------------------------------------------------
//
// Rule applications of the default variant (`--table`): 8 N - 1 for the
// midpoint scheme (per level: one row.ite, two row.store, two compare
// folds, ite.false / ite.same rules) and about 6 N for the mixed scheme;
// 319 at N = 40. The bound is c = 10 per level plus a constant: it leaves
// room for rule changes that add an application per level, and is far below
// any super-linear behavior.
constexpr unsigned kRulesPerLevel = 10;
constexpr unsigned kRulesConst = 16;
// slack of the doubling checks (N -> 2N): measured differences are 1
constexpr unsigned kDoublingSlack = 64;
// With chain-start caching off (and the follow-up pushed without a cache
// lookup, as in the SeaHorn branch) the work doubles per level: 393114 at
// N = 16 (about 2^N * 6), 6290978 at N = 20. The bound only needs to be
// clearly super-linear: 40 N^2 = 10240 at N = 16 against 127 linear.
constexpr unsigned kSuperLinearQuadratic = 40;

void testFig71Linear() {
  for (Scheme scheme : {Scheme::Midpoint, Scheme::Mixed}) {
    const char *sname =
        scheme == Scheme::Midpoint ? "fig71-midpoint" : "fig71-mixed";
    // the limit makes a regression fail here instead of running for hours
    Run r40 = runFig71(40, scheme, kDefault, 100 * 40);
    printRun(sname, kDefault, 40, r40);
    CHECK(!r40.aborted, sname << ": gave up at the rule-call limit");
    CHECK(r40.sound, sname << ": rewritten term differs from the original");
    CHECK(!r40.shape.hasStore, sname << ": a store is left in the result");
    CHECK(r40.stats.rulesFired <= kRulesPerLevel * 40 + kRulesConst,
          sname << ": rule applications at N=40 above c*N");
    CHECK(r40.stats.chainStartCaches > 0,
          sname << ": no chain start was cached");

    // linear growth: doubling N at most doubles the work (+ constant)
    Run r20 = runFig71(20, scheme, kDefault, 100 * 20);
    Run r80 = runFig71(80, scheme, kDefault, 100 * 80);
    printRun(sname, kDefault, 20, r20);
    printRun(sname, kDefault, 80, r80);
    CHECK(r40.stats.rulesFired <= 2 * r20.stats.rulesFired + kDoublingSlack,
          sname << ": super-linear growth from N=20 to N=40");
    CHECK(r80.stats.rulesFired <= 2 * r40.stats.rulesFired + kDoublingSlack,
          sname << ": super-linear growth from N=40 to N=80");
  }
}

// The ablation holds for the follow-up pushed directly (the SeaHorn branch
// and the default). With the follow-up visited through the cache (z3's way)
// the work is linear without chain-start caching too (kZ3Like, about 12% over
// the default), so the product must not switch followUpLookup on and rely on
// this test: it asserts the default below.
void testFig71NeedsChainStartCache() {
  CHECK(!RewriteOptions().followUpLookup,
        "ablation: the default visits follow-ups through the cache; the "
        "chain-start ablation below no longer describes the product");
  {
    // z3's way: linear without chain-start caching (measured, not required
    // by the thesis): documents that the dependency is on the direct push
    Run r = runFig71(40, Scheme::Midpoint, kZ3Like, 100 * 40);
    printRun("followup-lookup", kZ3Like, 40, r);
    CHECK(!r.aborted && r.sound, "followup-lookup: aborted or unsound");
    CHECK(r.stats.rulesFired <= kRulesPerLevel * 40 + kRulesConst,
          "followup-lookup: rule applications at N=40 above c*N");
  }
  for (Scheme scheme : {Scheme::Midpoint, Scheme::Mixed}) {
    const char *sname =
        scheme == Scheme::Midpoint ? "ablation-midpoint" : "ablation-mixed";
    const unsigned n = 16;
    Run on = runFig71(n, scheme, kDefault);
    Run off = runFig71(n, scheme, kNoChain);
    printRun(sname, kDefault, n, on);
    printRun(sname, kNoChain, n, off);
    // same result either way, only the work differs
    CHECK(off.sound, sname << ": no-chain-start result differs");
    CHECK(off.stats.chainStartCaches == 0,
          sname << ": chain starts cached with the switch off");
    CHECK(off.stats.rulesFired > kSuperLinearQuadratic * n * n,
          sname << ": without chain-start caching the work is not "
                   "super-linear; the linear test would not detect a "
                   "regression");
    CHECK(on.stats.rulesFired * 20 < off.stats.rulesFired,
          sname << ": chain-start caching does not pay");
  }
}

/// A store chain s_L = store(... store(m0, a_0, v_0) ..., a_{L-1}, v_{L-1})
/// read at numerals.
struct Chain {
  Expr m0, mem;
  std::vector<unsigned long> addr;
  Chain(Builder &b, unsigned len) {
    m0 = b.mem("m0");
    mem = m0;
    for (unsigned i = 0; i < len; ++i) {
      addr.push_back(2 * i + 1); // odd addresses; even ones are never stored
      mem = op::array::store(mem, b.num(2 * i + 1), b.num(100 + i));
    }
  }
};

void testLongChainReadTwice() {
  ExprFactory efac;
  Builder b(efac);
  const unsigned len = 200;
  Chain ch(b, len);
  std::vector<Expr> noConds;
  Expr read = op::array::select(ch.mem, b.num(1)); // the oldest store

  RewriteStats stats;
  RewriteCache cache;
  MinimalRewriteConfig config(efac, stats);
  ExprRewriter<MinimalRewriteConfig> rw(config, cache, stats);
  Expr res = rw.rewriteExpr(read);
  uint64_t firedFirst = stats.rulesFired;
  uint64_t callsFirst = stats.ruleCalls;
  llvm::outs() << "chain-twice first: rules-fired=" << firedFirst
               << " rule-calls=" << callsFirst << "\n";
  CHECK(res == b.num(100), "chain: read of the oldest store is its value");
  CHECK(firedFirst <= 3 * len + 8, "chain: first read above c*L");

  // second read of the same term: nothing is rewritten, all from the cache
  Expr res2 = rw.rewriteExpr(read);
  llvm::outs() << "chain-twice second: rules-fired="
               << stats.rulesFired - firedFirst
               << " rule-calls=" << stats.ruleCalls - callsFirst
               << " cache-hits=" << stats.cacheHits << "\n";
  CHECK(res2 == res, "chain: the second read differs");
  CHECK(stats.rulesFired == firedFirst, "chain: second read applied rules");
  CHECK(stats.ruleCalls == callsFirst, "chain: second read called the rules");

  // a read of a different address of the same memory is a new chain walk,
  // but the walk is linear again, and a read at an even address (never
  // stored) reaches the base memory
  RewriteStats s2;
  ExprRewriter<MinimalRewriteConfig> rw2(config, cache, s2);
  Expr miss = rw2.rewriteExpr(op::array::select(ch.mem, b.num(0)));
  CHECK(miss == op::array::select(ch.m0, b.num(0)),
        "chain: read of an unwritten address is a read of the base");
  CHECK(s2.rulesFired <= 3 * len + 8, "chain: miss read above c*L");
}

void testManyAddresses() {
  // the same memory (Fig. 7.1 shape) read at K addresses with one rewriter
  // cache: the work is about K times the work of one read, not exponential
  ExprFactory efac;
  Builder b(efac);
  const unsigned n = 30, kReads = 20;
  Expr m0 = b.mem("m0");
  std::vector<Expr> conds;
  Expr m = m0;
  for (unsigned k = 0; k < n; ++k) {
    Expr c = b.cond("c" + std::to_string(k));
    conds.push_back(c);
    m = mk<ITE>(c, op::array::store(m, b.num(k), b.num(5000 + k)),
                op::array::store(m, b.num(1000 + k), b.num(9000 + k)));
  }
  RewriteStats stats;
  RewriteCache cache;
  MinimalRewriteConfig config(efac, stats);
  RewriteOptions opts;
  opts.maxRuleCalls = 100 * n; // per read; a regression fails, not hangs
  ExprRewriter<MinimalRewriteConfig> rw(config, cache, stats, opts);
  for (unsigned j = 0; j < kReads; ++j) {
    Expr root = op::array::select(m, b.num(j));
    Expr res = rw.rewriteExpr(root);
    CHECK(!rw.aborted(), "many: read at " << j << " hit the rule-call limit");
    CHECK(agree(conds, m0, root, res), "many: read at " << j << " differs");
    CHECK(!shapeOf(res).hasStore, "many: store left");
  }
  llvm::outs() << "many-addresses: N=" << n << " reads=" << kReads
               << " rules-fired=" << stats.rulesFired
               << " cache-hits=" << stats.cacheHits << "\n";
  CHECK(stats.rulesFired <= (kRulesPerLevel * n + kRulesConst) * kReads,
        "many: rule applications above c*N*K");

  // the same reads again with the same cache: no rule application
  uint64_t before = stats.rulesFired;
  for (unsigned j = 0; j < kReads; ++j)
    rw.rewriteExpr(op::array::select(m, b.num(j)));
  CHECK(stats.rulesFired == before, "many: repeated reads applied rules");
}

void testCounters() {
  // per-rule counters are readable and add up
  Run r = runFig71(8, Scheme::Midpoint, kDefault);
  llvm::outs() << "counters N=8:\n";
  {
    sea::stats::Report rep;
    r.stats.report(rep, "  ");
    rep.print(llvm::outs(), false);
  }
  CHECK(r.stats.get("row.store") > 0, "counter row.store");
  CHECK(r.stats.get("row.ite") > 0, "counter row.ite");
  CHECK(r.stats.get("eq.bvnum") > 0, "counter eq.bvnum");
  CHECK(r.stats.get("ite.false") + r.stats.get("ite.true") > 0,
        "counter ite.true / ite.false");
  uint64_t perRule = 0;
  for (auto &kv : r.stats.rules)
    perRule += kv.getValue();
  // (rulesFired compares the rule's result with the rebuilt term, so a rebuild
  // of an ite with new kids is not a fired call: the sums are equal)
  CHECK(perRule == r.stats.rulesFired,
        "per-rule counts differ from rules-fired: a rule does not count");
}

void testRuleBasics() {
  ExprFactory efac;
  Builder b(efac);
  RewriteStats stats;
  RewriteCache cache;
  MinimalRewriteConfig config(efac, stats);
  ExprRewriter<MinimalRewriteConfig> rw(config, cache, stats);
  Expr m = b.mem("m");
  Expr c = b.cond("c");
  Expr t = mk<TRUE>(efac), f = mk<FALSE>(efac);

  CHECK(rw.rewriteExpr(mk<EQ>(b.num(3), b.num(3))) == t, "3 = 3");
  CHECK(rw.rewriteExpr(mk<EQ>(b.num(3), b.num(4))) == f, "3 = 4");
  CHECK(rw.rewriteExpr(mk<ITE>(t, b.num(1), b.num(2))) == b.num(1), "ite t");
  CHECK(rw.rewriteExpr(mk<ITE>(f, b.num(1), b.num(2))) == b.num(2), "ite f");
  CHECK(rw.rewriteExpr(mk<ITE>(c, b.num(7), b.num(7))) == b.num(7),
        "ite equal arms");
  // a compare of a constant with a numeral is left alone
  Expr x = bind::mkConst(mkTerm<std::string>("x", efac), b.bvSort);
  Expr cmp = mk<EQ>(x, b.num(1));
  CHECK(rw.rewriteExpr(cmp) == cmp, "x = 1 is not rewritten");
  // read over store with a symbolic index: ROW, the compare stays
  Expr st = op::array::store(m, x, b.num(9));
  Expr rd = rw.rewriteExpr(op::array::select(st, b.num(2)));
  CHECK(rd == mk<ITE>(mk<EQ>(b.num(2), x), b.num(9),
                      op::array::select(m, b.num(2))),
        "ROW with a symbolic index");
  // read over ite of arrays
  Expr m2 = b.mem("m2");
  Expr ia = mk<ITE>(c, m, m2);
  Expr rd2 = rw.rewriteExpr(op::array::select(ia, b.num(2)));
  CHECK(rd2 == mk<ITE>(c, op::array::select(m, b.num(2)),
                       op::array::select(m2, b.num(2))),
        "select over ite of arrays");
  // the result of an unrewritable root is the root
  CHECK(rw.rewriteExpr(m) == m, "terminal root");
}

/// A config of hand-made rules, to test the traversal itself: constants of
/// the map are rewritten to the given term with the given status; ites by the
/// ITE rules; every other application is visited and left alone.
struct HandConfig : ExprRewriterConfigBase {
  rules::ITERewriteRule iteRule;
  std::map<ENode *, std::pair<Expr, RWStatus>> step;
  HandConfig(ExprFactory &efac, RewriteStats &stats)
      : ExprRewriterConfigBase(efac, stats), iteRule(stats) {}
  bool shouldRewrite(const Expr &e) const {
    return step.count(e.get()) || !rules::isLeaf(e);
  }
  RewriteResult doRewrite(const Expr &e) {
    auto it = step.find(e.get());
    if (it != step.end())
      return {it->second.first, it->second.second};
    if (isOpX<ITE>(e))
      return iteRule(e);
    return {e, RWStatus::RW_DONE};
  }
};

void testMultiStepCacheEntry() {
  // a ~> b ~> c: the cache maps the start a to c (Alg. 4-5), and also the
  // last b to c (Alg. 2 lines 20-21); with the switch off, not a
  for (bool chainStart : {true, false}) {
    ExprFactory efac;
    Builder b(efac);
    RewriteStats stats;
    RewriteCache cache;
    HandConfig config(efac, stats);
    Expr a = b.cond("a"), bb = b.cond("b"), c = b.cond("c"), d = b.cond("d");
    config.step[a.get()] = {bb, RWStatus::RW_1};
    config.step[bb.get()] = {c, RWStatus::RW_DONE};
    RewriteOptions opts;
    opts.chainStartCache = chainStart;
    ExprRewriter<HandConfig> rw(config, cache, stats, opts);
    Expr res = rw.rewriteExpr(mk<AND>(a, d));
    CHECK(res == mk<AND>(c, d), "multi-step: result");
    const Expr *ca = cache.find(a, kFullDepth);
    const Expr *cb = cache.find(bb, 1);
    CHECK(chainStart == (ca && *ca == c),
          "multi-step: cache[a] is c exactly with chain-start caching");
    CHECK(cb && *cb == c, "multi-step: cache[b] is c");
    // the second visit of a costs no rule call
    uint64_t calls = stats.ruleCalls;
    Expr e = b.cond("e");
    CHECK(rw.rewriteExpr(mk<AND>(a, e)) == mk<AND>(c, e),
          "multi-step: second result");
    CHECK(chainStart ? stats.ruleCalls == calls + 1
                     : stats.ruleCalls > calls + 1,
          "multi-step: the second visit of a re-ran its chain (or not)");
  }
}

/// Smallest term with the redex ite(true, 1, 2) at level \p d below the root;
/// the other levels are ite(c, ., 0), which no rule touches.
Expr redexAt(Builder &b, unsigned d, bool reduced) {
  Expr c = b.cond("c");
  Expr t = reduced ? b.num(1) : mk<ITE>(mk<TRUE>(b.efac), b.num(1), b.num(2));
  for (unsigned i = 0; i < d; ++i)
    t = mk<ITE>(c, t, b.num(0));
  return t;
}

void testDepthSemantics() {
  // RW_n: the rules are applied at the follow-up and n levels below it
  // (Alg. 2 line 25 of the thesis: the follow-up frame has depth n)
  struct {
    RWStatus st;
    unsigned n;
  } cases[] = {{RWStatus::RW_1, 1}, {RWStatus::RW_2, 2}, {RWStatus::RW_3, 3}};
  for (auto &cs : cases) {
    for (unsigned d = 0; d <= 5; ++d) {
      for (bool lookup : {false, true}) {
        ExprFactory efac;
        Builder b(efac);
        RewriteStats stats;
        RewriteCache cache;
        HandConfig config(efac, stats);
        Expr r = b.cond("r");
        config.step[r.get()] = {redexAt(b, d, false), cs.st};
        RewriteOptions opts;
        opts.followUpLookup = lookup;
        ExprRewriter<HandConfig> rw(config, cache, stats, opts);
        Expr res = rw.rewriteExpr(r);
        bool reduced = d <= cs.n;
        CHECK(res == redexAt(b, d, reduced),
              "depth: RW_" << cs.n << " redex at level " << d
                           << (lookup ? " (lookup)" : "") << ": "
                           << (reduced ? "must be reduced" : "must be left"));
      }
    }
  }
  // RW_FULL: any level
  ExprFactory efac;
  Builder b(efac);
  RewriteStats stats;
  RewriteCache cache;
  HandConfig config(efac, stats);
  Expr r = b.cond("r");
  config.step[r.get()] = {redexAt(b, 9, false), RWStatus::RW_FULL};
  ExprRewriter<HandConfig> rw(config, cache, stats);
  CHECK(rw.rewriteExpr(r) == redexAt(b, 9, true), "depth: RW_FULL level 9");
}

void testBoundedResultNotTakenAsFull() {
  // A term first visited under a depth bound is cached with that bound; a
  // later full visit of it does not take that entry (it was poisoned before:
  // a shared V seen at the edge of a ROW follow-up kept its redex, and the
  // full visit took it from the cache)
  ExprFactory efac;
  Builder b(efac);
  RewriteStats stats;
  RewriteCache cache;
  HandConfig config(efac, stats);
  Expr r = b.cond("r");
  Expr inner = mk<ITE>(b.cond("c3"),
                       mk<ITE>(mk<TRUE>(efac), b.num(10), b.num(20)), b.num(0));
  // inner sits at level 1 below the follow-up of r: visited with its kids
  // raw, so the redex below it stays
  Expr follow = mk<ITE>(b.cond("c1"), inner, b.num(0));
  config.step[r.get()] = {follow, RWStatus::RW_1};
  ExprRewriter<HandConfig> rw(config, cache, stats);
  Expr first = rw.rewriteExpr(r);
  CHECK(first == follow, "bounded: the bounded visit left the term as it is");
  // the full visit of the term itself must reduce it
  Expr full = rw.rewriteExpr(inner);
  CHECK(full == mk<ITE>(b.cond("c3"), b.num(10), b.num(0)),
        "bounded: a later full visit takes a bounded entry");
}

void testReadOverWriteNormalizesKids() {
  ExprFactory efac;
  Builder b(efac);
  RewriteStats stats;
  RewriteCache cache;
  MinimalRewriteConfig config(efac, stats);
  ExprRewriter<MinimalRewriteConfig> rw(config, cache, stats);
  Expr m = b.mem("m");
  Expr t = mk<TRUE>(efac);
  // an index that is a redex: the compare folds after normalization
  Expr idx = mk<ITE>(t, b.num(0), b.num(1));
  CHECK(rw.rewriteExpr(op::array::select(op::array::store(m, idx, b.num(9)),
                                         b.num(0))) == b.num(9),
        "row: redex in the store index");
  // a value that is a redex, read twice (once in a shared position)
  Expr v = mk<ITE>(mk<EQ>(b.num(1), b.num(1)), b.num(10), b.num(20));
  Expr c = b.cond("c");
  Expr root = mk<ITE>(
      c, op::array::select(op::array::store(m, b.num(0), v), b.num(0)), v);
  CHECK(rw.rewriteExpr(root) == b.num(10), "row: shared redex value");
  CHECK(rw.rewriteExpr(v) == b.num(10), "row: shared redex value, again");
  // a read under a store value is reached (store is visited)
  Expr m2 = b.mem("m2");
  Expr inner =
      op::array::select(op::array::store(m, b.num(3), b.num(4)), b.num(3));
  CHECK(rw.rewriteExpr(op::array::store(m2, b.num(0), inner)) ==
            op::array::store(m2, b.num(0), b.num(4)),
        "row: a read in a store value");
}

void testEqModuloWidth() {
  // numerals are compared as the solvers read them: modulo 2^width
  ExprFactory efac;
  Builder b(efac);
  RewriteStats stats;
  RewriteCache cache;
  MinimalRewriteConfig config(efac, stats);
  ExprRewriter<MinimalRewriteConfig> rw(config, cache, stats);
  Expr t = mk<TRUE>(efac), f = mk<FALSE>(efac);
  auto num = [&](const char *dec, unsigned w) {
    return bv::bvnum(mpz_class(dec), w, efac);
  };
  CHECK(rw.rewriteExpr(mk<EQ>(num("-1", 32), num("4294967295", 32))) == t,
        "eq: -1 = 2^32 - 1");
  CHECK(rw.rewriteExpr(mk<EQ>(num("4294967296", 32), num("0", 32))) == t,
        "eq: 2^32 = 0");
  CHECK(rw.rewriteExpr(mk<EQ>(num("-4294967296", 32), num("0", 32))) == t,
        "eq: -2^32 = 0");
  CHECK(rw.rewriteExpr(mk<EQ>(num("-1", 32), num("1", 32))) == f, "eq: -1 = 1");
  CHECK(rw.rewriteExpr(mk<EQ>(num("255", 8), num("-1", 8))) == t,
        "eq: 255 = -1 (8 bits)");
  // different widths: not a well-sorted compare, left alone
  Expr bad = mk<EQ>(num("1", 8), num("1", 16));
  CHECK(rw.rewriteExpr(bad) == bad, "eq: different widths are not folded");
}

/// The measurement table: rule applications per variant and N.
void table() {
  llvm::outs() << "table: rules-fired of Fig. 7.1 scaled, scheme midpoint\n";
  const Variant *variants[] = {&kDefault, &kChainLookup, &kNoChain,
                               &kZ3Like,  &kSharedOnly,  &kNoChainShared};
  for (const Variant *v : variants) {
    // without chain-start caching and without the follow-up lookup the work
    // is exponential (N = 20: 6.3 million applications, about a minute)
    bool exponential = !v->chainStartCache && !v->followUpLookup;
    for (unsigned n : {4u, 8u, 12u, 16u, 20u}) {
      if (exponential && n > 16)
        continue;
      Run r = runFig71(n, Scheme::Midpoint, *v);
      CHECK(r.sound, "table: unsound " << v->name << " N=" << n);
      printRun("table", *v, n, r);
    }
  }
  for (unsigned n : {20u, 40u, 80u, 160u}) {
    Run r = runFig71(n, Scheme::Midpoint, kDefault);
    printRun("table-default", kDefault, n, r);
  }
}

} // namespace

int main(int argc, char **argv) {
  bool printTable = argc > 1 && std::string(argv[1]) == "--table";
  testRuleBasics();
  testCounters();
  testFig71Linear();
  testFig71NeedsChainStartCache();
  testLongChainReadTwice();
  testManyAddresses();
  testMultiStepCacheEntry();
  testDepthSemantics();
  testBoundedResultNotTakenAsFull();
  testReadOverWriteNormalizesKids();
  testEqModuloWidth();
  if (printTable)
    table();
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
