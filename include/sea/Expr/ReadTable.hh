// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// EXPERIMENT (prototype for data, not a production design). Reached only
// through the hidden `sea-vc --x-read-table=<mode>`; nothing else uses it.
//
// A guarded read table built at the solver boundary instead of the
// semantics-only MemOpTranslator: every read select(M, j) is split over the
// leaves of its address (ites and +/-/neg/mul-by-numeral distributed, literal
// sets kept consistent), and pushed down M per leaf; at a memory-operation
// term the destination is split too and the copy-hit test is made per leaf
// pair, with the base cancelled when both leaves are in one object; the
// translated source read keeps the source object and offset. Every read that
// reaches an array constant A becomes a record (A, object or unknown, offset,
// address), the node select(A, base + off). Values are exact (identities of
// bit-vector arithmetic and of select over ite), except for branches that
// A-INB makes irrelevant (a read leaf in a size-0 object, a hit between two
// different objects, a source leaf in a size-0 object): those values are fresh
// unconstrained constants (`xrt.dead.<n>`), a weakening, with the premise
// recorded (Dead::kind).
//
// Relevance: a top-down pass over the translated query gives each node the
// condition under which its value can matter (ite branches, implication
// consequents), as a term and as a capped DNF of literals (for syntactic
// exclusion).
//
// Lowering (mode): records of one array are partitioned into buckets by
// object; the functional-consistency constraints are
//   flat   : every pair of the array, addr_k = addr_l -> r_k = r_l (Ackermann
//            on the split reads)
//   objack : pairs within one object bucket, off_k = off_l -> r_k = r_l;
//            pairs across two object buckets omitted (A-INB + layout); an
//            unknown-provenance record is linked to every record of its array
//   arrays : one array per (A, object), read at the offset; unknown records
//            as in objack
//   mux    : objects of constant size <= muxMax cells: one constant per cell,
//            a read is ite(off <u size, mux(off), r_out); others as objack
//   plain  : no lowering (the split reads go to the solver's arrays)
// Pairs dropped exactly: same symbolic offset part, different constants;
// syntactically exclusive relevance. Omitted pairs (different objects) and
// dead branches are listed for the premise checks of sea-vc.
#include "sea/Expr/Ackermann.hh"
#include "sea/Expr/BaseRegistry.hh"
#include "sea/Expr/MemOps.hh"
#include "sea/Expr/SeaMRules.hh"
#include "sea/Expr/Smt/Model.hh"
#include "sea/Expr/TypeChecker.hh"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace expr {
namespace xrt {

struct IdLess {
  bool operator()(const ENode *a, const ENode *b) const {
    return a->getId() < b->getId();
  }
};

/// A conjunction of literals (condition node, polarity), sorted by node id.
using Lits = std::vector<std::pair<ENode *, bool>>;

inline bool mergeLits(const Lits &a, const Lits &b, Lits &out) {
  out.clear();
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i].first == b[j].first) {
      if (a[i].second != b[j].second)
        return false;
      out.push_back(a[i]);
      ++i;
      ++j;
    } else if (a[i].first->getId() < b[j].first->getId()) {
      out.push_back(a[i++]);
    } else {
      out.push_back(b[j++]);
    }
  }
  while (i < a.size())
    out.push_back(a[i++]);
  while (j < b.size())
    out.push_back(b[j++]);
  return true;
}

inline std::pair<ENode *, bool> mkLit(ENode *c, bool pos) {
  while (isOpX<NEG>(c) && c->arity() == 1) {
    c = c->arg(0);
    pos = !pos;
  }
  return {c, pos};
}

inline Expr litExpr(const std::pair<ENode *, bool> &l) {
  return l.second ? Expr(l.first) : mk<NEG>(Expr(l.first));
}

/// The literals of \p b that are not in \p a.
inline Lits minusLits(const Lits &b, const Lits &a) {
  Lits r;
  for (const auto &l : b)
    if (std::find(a.begin(), a.end(), l) == a.end())
      r.push_back(l);
  return r;
}

inline Expr conjOf(const Lits &ls, ExprFactory &ef) {
  if (ls.empty())
    return mk<TRUE>(ef);
  if (ls.size() == 1)
    return litExpr(ls[0]);
  ExprVector v;
  for (const auto &l : ls)
    v.push_back(litExpr(l));
  return mknary<AND>(v.begin(), v.end());
}

/// A linear form modulo 2^w: sum coef(a) * a + c.
struct Lin {
  std::map<ENode *, mpz_class, IdLess> coef;
  mpz_class c;
  bool operator==(const Lin &o) const { return coef == o.coef && c == o.c; }
};

class LinOps {
public:
  explicit LinOps(unsigned w) : m_w(w) {
    mpz_ui_pow_ui(m_mod.get_mpz_t(), 2, w);
  }
  unsigned width() const { return m_w; }
  mpz_class norm(const mpz_class &x) const {
    mpz_class r;
    mpz_fdiv_r(r.get_mpz_t(), x.get_mpz_t(), m_mod.get_mpz_t());
    return r;
  }
  /// acc += k * l
  void addScaled(Lin &acc, const Lin &l, const mpz_class &k) const {
    acc.c = norm(acc.c + l.c * k);
    for (const auto &[a, v] : l.coef) {
      mpz_class s = norm(acc.coef[a] + v * k);
      if (s == 0)
        acc.coef.erase(a);
      else
        acc.coef[a] = s;
    }
  }
  mpz_class minusOne() const { return m_mod - 1; }
  Lin sub(const Lin &a, const Lin &b) const {
    Lin r = a;
    addScaled(r, b, minusOne());
    return r;
  }
  Lin add(const Lin &a, const Lin &b) const {
    Lin r = a;
    addScaled(r, b, mpz_class(1UL));
    return r;
  }
  /// The term of \p l (atoms in id order, then the numeral).
  Expr rebuild(const Lin &l, ExprFactory &ef) const {
    std::vector<Expr> ts;
    for (const auto &[a, v] : l.coef)
      ts.push_back(v == 1 ? Expr(a)
                          : mk<BMUL>(op::bv::bvnum(v, m_w, ef), Expr(a)));
    if (l.c != 0 || ts.empty())
      ts.push_back(op::bv::bvnum(l.c, m_w, ef));
    return ts.size() == 1 ? ts[0] : mknary<BADD>(ts.begin(), ts.end());
  }

private:
  unsigned m_w;
  mpz_class m_mod;
};

/// One leaf of a term: under the literals \p lits the term equals \p lin.
struct SLeaf {
  Lits lits;
  Lin lin;
};
using Leaves = std::vector<SLeaf>;

/// Leaves of w-bit terms: ites split (literal sets merged, contradictory
/// ones dropped), + - neg and mul by numerals distributed. A term with more
/// than \p cap leaves is one atom (unsplit).
class Splitter {
public:
  Splitter(unsigned w, unsigned cap) : m_ops(w), m_cap(cap) {}
  const LinOps &ops() const { return m_ops; }
  uint64_t overCap() const { return m_overCap; }

  const Leaves &of(ENode *n) {
    auto it = m_memo.find(n);
    if (it != m_memo.end())
      return *it->second;
    auto ls = std::make_unique<Leaves>(compute(n));
    m_keep.push_back(Expr(n));
    return *m_memo.emplace(n, std::move(ls)).first->second;
  }

private:
  Leaves atom(ENode *n) {
    SLeaf l;
    l.lin.coef[n] = 1;
    return {l};
  }
  Leaves compute(ENode *n) {
    Expr e(n);
    if (op::bv::is_bvnum(e)) {
      SLeaf l;
      l.lin.c = m_ops.norm(op::bv::toMpz(e));
      return {l};
    }
    if (isOpX<ITE>(n) && n->arity() == 3) {
      Leaves out;
      for (int b = 1; b <= 2; ++b) {
        Lits one{mkLit(n->arg(0), b == 1)}, m;
        for (const SLeaf &l : of(n->arg(b)))
          if (mergeLits(l.lits, one, m))
            out.push_back(SLeaf{m, l.lin});
      }
      if (out.size() > m_cap) {
        ++m_overCap;
        return atom(n);
      }
      return out;
    }
    if (isOpX<BADD>(n) || isOpX<BSUB>(n) ||
        (isOpX<BNEG>(n) && n->arity() == 1)) {
      Leaves acc{SLeaf{}};
      for (size_t i = 0; i < n->arity(); ++i) {
        mpz_class k = (isOpX<BNEG>(n) || (isOpX<BSUB>(n) && i > 0))
                          ? m_ops.minusOne()
                          : mpz_class(1UL);
        const Leaves &kl = of(n->arg(i));
        Leaves nxt;
        Lits m;
        for (const SLeaf &a : acc)
          for (const SLeaf &b : kl)
            if (mergeLits(a.lits, b.lits, m)) {
              SLeaf s{m, a.lin};
              m_ops.addScaled(s.lin, b.lin, k);
              nxt.push_back(std::move(s));
            }
        if (nxt.size() > m_cap) {
          ++m_overCap;
          return atom(n);
        }
        acc = std::move(nxt);
      }
      return acc;
    }
    if (isOpX<BMUL>(n)) {
      mpz_class k(1UL);
      ENode *other = nullptr;
      unsigned others = 0;
      for (size_t i = 0; i < n->arity(); ++i) {
        Expr kid(n->arg(i));
        if (op::bv::is_bvnum(kid))
          k = m_ops.norm(k * op::bv::toMpz(kid));
        else {
          other = n->arg(i);
          ++others;
        }
      }
      if (others == 0) {
        SLeaf l;
        l.lin.c = k;
        return {l};
      }
      if (others > 1)
        return atom(n);
      Leaves out;
      for (const SLeaf &l : of(other)) {
        SLeaf s{l.lits, Lin()};
        m_ops.addScaled(s.lin, l.lin, k);
        out.push_back(std::move(s));
      }
      return out;
    }
    return atom(n);
  }

  LinOps m_ops;
  unsigned m_cap;
  uint64_t m_overCap = 0;
  std::unordered_map<ENode *, std::unique_ptr<Leaves>> m_memo;
  std::vector<Expr> m_keep;
};

/// An address leaf with its provenance: base (a registered object) + offset,
/// or unknown (no base, several bases, a base under another operator).
struct ALeaf {
  Lits lits;
  Lin full;  ///< the whole address
  Expr base; ///< null: unknown provenance
  Lin off;   ///< full minus base (== full when unknown)
  Expr offE, addr;
};

struct Record {
  Expr array; ///< the array constant A
  Expr sel;   ///< select(A, addr): the node in the translated query
  ALeaf leaf;
  bool extra; ///< made after freeze() (only by a check term)
};

struct Dead {
  Expr sym;
  const char *kind;
};

struct TranslatorStats {
  uint64_t reads = 0, leafSum = 0, leafMax = 0, rCalls = 0, rMemo = 0;
  uint64_t stores = 0, storePairs = 0, memops = 0, memopPairs = 0;
  uint64_t samePairs = 0, unknownPairs = 0, decided = 0, other = 0;
  uint64_t srcLeaves = 0, srcUnknown = 0;
};

/// The translation (see the header). One per query; keeps every term alive.
class Translator {
public:
  Translator(ExprFactory &ef, const BaseRegistry &bases,
             const seam::Rules &rules, unsigned cap, bool inb)
      : m_ef(ef), m_bases(bases), m_rules(rules), m_cap(cap), m_inb(inb) {}

  /// \p e with every read pushed down to records (store-maps must be
  /// expanded before). Null if a memory term is in a position that is not
  /// read (\p why).
  Expr translate(const Expr &e, std::string &why) {
    m_why.clear();
    Expr r = T(e);
    if (!m_why.empty()) {
      why = m_why;
      return Expr();
    }
    return r;
  }
  /// Records made from now on are `extra` (check terms only).
  void freeze() { m_frozen = true; }

  const std::vector<Record> &records() const { return m_records; }
  const std::vector<Dead> &deads() const { return m_deads; }
  const TranslatorStats &stats() const { return m_stats; }
  long recordOf(ENode *sel) const {
    auto it = m_recIndex.find(sel);
    return it == m_recIndex.end() ? -1 : long(it->second);
  }
  uint64_t overCap() const {
    uint64_t n = 0;
    for (auto &[w, s] : m_split)
      n += s->overCap();
    return n;
  }

private:
  Splitter &splitter(unsigned w) {
    auto it = m_split.find(w);
    if (it == m_split.end())
      it = m_split.emplace(w, std::make_unique<Splitter>(w, m_cap)).first;
    return *it->second;
  }

  bool mentionsBase(ENode *a) const { return m_rules.mentionsBase(Expr(a)); }

  ALeaf classify(const Lits &lits, const Lin &full, const LinOps &ops) {
    ALeaf l;
    l.lits = lits;
    l.full = full;
    ENode *base = nullptr;
    bool unknown = false;
    for (const auto &[a, v] : full.coef) {
      if (m_bases.isBase(Expr(a))) {
        if (base || v != 1)
          unknown = true;
        else
          base = a;
      } else if (mentionsBase(a)) {
        unknown = true;
      }
    }
    if (base && !unknown) {
      l.base = Expr(base);
      l.off = full;
      l.off.coef.erase(base);
      l.offE = ops.rebuild(l.off, m_ef);
      l.addr = (l.off.coef.empty() && l.off.c == 0)
                   ? l.base
                   : Expr(mk<BADD>(l.base, l.offE));
    } else {
      l.off = full;
      l.offE = ops.rebuild(full, m_ef);
      l.addr = l.offE;
    }
    m_keepE.push_back(l.addr);
    return l;
  }

  std::vector<ALeaf> addrLeaves(const Expr &j) {
    unsigned w = m_rules.widthOf(j);
    Splitter &sp = splitter(w);
    std::vector<ALeaf> out;
    for (const SLeaf &s : sp.of(j.get()))
      out.push_back(classify(s.lits, s.lin, sp.ops()));
    return out;
  }

  bool size0(const Expr &base) const {
    const ObjectBase *o = m_bases.lookup(base);
    return o && o->size.isZero();
  }

  Expr cellSort(const Expr &mem) {
    Expr s = m_tc.typeOf(mem);
    return sort::arrayValTy(s);
  }

  Expr dead(const Expr &mem, const char *kind) {
    std::string name = "xrt.dead." + std::to_string(m_deads.size());
    Expr c = bind::mkConst(mkTerm<std::string>(name, m_ef), cellSort(mem));
    m_deads.push_back(Dead{c, kind});
    return c;
  }

  bool hasSel(ENode *n) {
    auto it = m_hasSel.find(n);
    if (it != m_hasSel.end())
      return it->second;
    bool h = false;
    if (!ackermann_detail::isLeaf(n)) {
      h = isOpX<SELECT>(n) || isMemOpTerm(Expr(n));
      for (size_t i = 0; i < n->arity() && !h; ++i)
        h = hasSel(n->arg(i));
    }
    m_keepE.push_back(Expr(n));
    m_hasSel.emplace(n, h);
    return h;
  }

  Expr T(const Expr &e) {
    ENode *n = e.get();
    if (ackermann_detail::isLeaf(n) || !hasSel(n))
      return e;
    auto it = m_memoT.find(n);
    if (it != m_memoT.end())
      return it->second;
    Expr r;
    if (isOpX<SELECT>(n)) {
      Expr j = T(Expr(n->arg(1)));
      r = readTop(Expr(n->arg(0)), j);
    } else if (isMemoryHead(e) && !isOpX<FAPP>(n)) {
      m_why = std::string("a memory term (") + n->op().name() +
              ") in a position that is not read";
      r = e;
    } else {
      llvm::SmallVector<Expr, 4> kids;
      for (size_t i = 0; i < n->arity(); ++i)
        kids.push_back(T(Expr(n->arg(i))));
      r = ackermann_detail::withKids(n, kids);
    }
    m_keepE.push_back(e);
    m_memoT.emplace(n, r);
    return r;
  }

  /// ite chain over mutually exclusive, exhaustive leaves (the last one is
  /// the default); literals of \p ctx hold already.
  Expr chain(const Lits &ctx, const std::vector<Lits> &lits,
             const std::vector<Expr> &vals) {
    Expr res = vals.back();
    for (size_t i = vals.size() - 1; i-- > 0;) {
      Expr g = conjOf(minusLits(lits[i], ctx), m_ef);
      res = isOpX<TRUE>(g) ? vals[i] : Expr(mk<ITE>(g, vals[i], res));
    }
    return res;
  }

  Expr readTop(const Expr &mem, const Expr &j) {
    ++m_stats.reads;
    std::vector<ALeaf> ls = addrLeaves(j);
    m_stats.leafSum += ls.size();
    m_stats.leafMax = std::max<uint64_t>(m_stats.leafMax, ls.size());
    const bool shadow = m_rules.shadowMemory(mem);
    std::vector<Lits> lits;
    std::vector<Expr> vals;
    for (const ALeaf &l : ls) {
      lits.push_back(l.lits);
      if (!shadow && m_inb && l.base && size0(l.base))
        vals.push_back(dead(mem, "access-size0")); // A-INB of the access
      else
        vals.push_back(R(mem, l));
    }
    return chain(Lits(), lits, vals);
  }

  struct Key {
    ENode *m, *a;
    unsigned l;
    bool operator==(const Key &o) const {
      return m == o.m && a == o.a && l == o.l;
    }
  };
  struct KeyHash {
    size_t operator()(const Key &k) const {
      return std::hash<ENode *>()(k.m) * 31 + std::hash<ENode *>()(k.a) * 7 +
             k.l;
    }
  };
  unsigned litsId(const Lits &l) {
    return m_litsIds.emplace(l, m_litsIds.size()).first->second;
  }

  Expr record(const Expr &arr, const ALeaf &l) {
    Expr sel = op::array::select(arr, l.addr);
    if (!m_recIndex.count(sel.get())) {
      m_recIndex.emplace(sel.get(), m_records.size());
      m_records.push_back(Record{arr, sel, l, m_frozen});
    }
    return sel;
  }

  Expr R(const Expr &mem, const ALeaf &l) {
    ++m_stats.rCalls;
    Key key{mem.get(), l.addr.get(), litsId(l.lits)};
    auto it = m_memoR.find(key);
    if (it != m_memoR.end()) {
      ++m_stats.rMemo;
      return it->second;
    }
    Expr res = Rcompute(mem, l);
    m_keepE.push_back(mem);
    m_keepE.push_back(res);
    m_memoR.emplace(key, res);
    return res;
  }

  Expr Rcompute(const Expr &mem, const ALeaf &l) {
    ENode *m = mem.get();
    if (ackermann_detail::isArrayConstant(m))
      return record(mem, l);
    const unsigned w = l.full.coef.empty() && l.full.c == 0
                           ? m_rules.widthOf(l.addr)
                           : m_rules.widthOf(l.addr);
    Splitter &sp = splitter(w);
    const LinOps &ops = sp.ops();
    const bool shadow = m_rules.shadowMemory(mem);
    if (isOpX<STORE>(m)) {
      ++m_stats.stores;
      Expr i = T(Expr(m->arg(1))), v = T(Expr(m->arg(2)));
      std::vector<Lits> lits;
      std::vector<Expr> conds, vals;
      Lits mm;
      bool definite = false;
      Expr defVal;
      for (const ALeaf &il : addrLeaves(i)) {
        if (!mergeLits(l.lits, il.lits, mm))
          continue;
        ++m_stats.storePairs;
        Lin k = ops.sub(l.full, il.full);
        Expr eq, val = v;
        if (k.coef.empty()) {
          ++m_stats.decided;
          if (k.c != 0)
            continue; // different numerals: skip
          eq = mk<TRUE>(m_ef);
        } else if (!shadow && m_inb && l.base && il.base && l.base != il.base) {
          eq = mk<EQ>(l.addr, il.addr);
          val = dead(mem, "store-cross"); // A-INB of both accesses
        } else if (l.base && il.base && l.base == il.base) {
          ++m_stats.samePairs;
          eq = mk<EQ>(l.offE, il.offE);
        } else {
          ++m_stats.unknownPairs;
          eq = mk<EQ>(l.addr, il.addr);
        }
        Expr g = conjOf(minusLits(mm, l.lits), m_ef);
        Expr c = isOpX<TRUE>(eq)  ? g
                 : isOpX<TRUE>(g) ? eq
                                  : Expr(mk<AND>(g, eq));
        if (isOpX<TRUE>(c)) {
          definite = true;
          defVal = val;
          break;
        }
        conds.push_back(c);
        vals.push_back(val);
      }
      if (definite && conds.empty())
        return defVal;
      Expr res = definite ? defVal : R(Expr(m->arg(0)), l);
      for (size_t x = conds.size(); x-- > 0;)
        res = mk<ITE>(conds[x], vals[x], res);
      return res;
    }
    if (isOpX<ITE>(m)) {
      Expr c = T(Expr(m->arg(0)));
      Expr t = R(Expr(m->arg(1)), l), f = R(Expr(m->arg(2)), l);
      return t == f ? t : Expr(mk<ITE>(c, t, f));
    }
    if (isMemOpTerm(mem)) {
      ++m_stats.memops;
      Expr d = T(memOpDst(mem)), n = T(memOpLen(mem));
      std::vector<Expr> conds, vals;
      Lits mm;
      for (const ALeaf &dl : addrLeaves(d)) {
        if (!mergeLits(l.lits, dl.lits, mm))
          continue;
        ++m_stats.memopPairs;
        Lin k = ops.sub(l.full, dl.full);
        bool kBase = false;
        for (const auto &[a, v] : k.coef)
          kBase = kBase || m_bases.isBase(Expr(a)) || mentionsBase(a);
        Expr kE = ops.rebuild(k, m_ef);
        Expr hit;
        unsigned nw;
        if (k.coef.empty() && op::bv::isBvNum(n, nw)) {
          ++m_stats.decided;
          mpz_class nv = ackermann_detail::reducedValue(n);
          if (!(k.c < nv))
            continue; // out of the range: skip
          hit = mk<TRUE>(m_ef);
        } else {
          hit = mk<BULT>(kE, n);
        }
        Expr val;
        if (!shadow && m_inb && dl.base && size0(dl.base)) {
          val = dead(mem, "dst-size0"); // A-INB of the range: n = 0
        } else if (!shadow && m_inb && l.base && dl.base && l.base != dl.base) {
          val = dead(mem, "copy-cross"); // A-INB: access and range
        } else {
          ++(kBase ? m_stats.unknownPairs : m_stats.samePairs);
          if (isOpX<MEM_SET>(mem)) {
            val = T(Expr(m->arg(3)));
          } else if (isOpX<MEM_HAVOC>(mem)) {
            ALeaf hl = l;
            hl.lits = mm;
            val = R(Expr(m->arg(1)), hl);
          } else {
            Expr s = T(Expr(m->arg(3)));
            Expr ms = Expr(m->arg(1));
            const bool sshadow = m_rules.shadowMemory(ms);
            std::vector<Lits> slits;
            std::vector<Expr> svals;
            Lits m2;
            for (const ALeaf &sl : addrLeaves(s)) {
              if (!mergeLits(mm, sl.lits, m2))
                continue;
              ++m_stats.srcLeaves;
              ALeaf src = classify(m2, ops.add(sl.full, k), ops);
              if (!src.base)
                ++m_stats.srcUnknown;
              slits.push_back(m2);
              if (!sshadow && m_inb && src.base && size0(src.base))
                svals.push_back(dead(ms, "src-size0")); // A-INB of the source
              else
                svals.push_back(R(ms, src));
            }
            if (svals.empty()) {
              // every source leaf contradicts: the hit cannot happen here
              continue;
            }
            val = chain(mm, slits, svals);
          }
        }
        Expr g = conjOf(minusLits(mm, l.lits), m_ef);
        Expr c = isOpX<TRUE>(hit) ? g
                 : isOpX<TRUE>(g) ? hit
                                  : Expr(mk<AND>(g, hit));
        conds.push_back(c);
        vals.push_back(val);
      }
      Expr res = R(memOpBase(mem), l);
      for (size_t x = conds.size(); x-- > 0;)
        res = isOpX<TRUE>(conds[x]) ? vals[x]
                                    : Expr(mk<ITE>(conds[x], vals[x], res));
      return res;
    }
    ++m_stats.other;
    return op::array::select(mem, l.addr);
  }

  ExprFactory &m_ef;
  const BaseRegistry &m_bases;
  const seam::Rules &m_rules;
  unsigned m_cap;
  bool m_inb;
  bool m_frozen = false;
  std::string m_why;
  TypeChecker m_tc;
  std::map<unsigned, std::unique_ptr<Splitter>> m_split;
  std::unordered_map<ENode *, bool> m_hasSel;
  std::unordered_map<ENode *, Expr> m_memoT;
  std::unordered_map<Key, Expr, KeyHash> m_memoR;
  std::map<Lits, unsigned> m_litsIds;
  std::vector<Record> m_records;
  std::unordered_map<ENode *, size_t> m_recIndex;
  std::vector<Dead> m_deads;
  std::vector<Expr> m_keepE;
  TranslatorStats m_stats;
};

/// Relevance of every node of a query (see the header).
class Relevance {
public:
  static constexpr unsigned kDnfCap = 16;
  struct Dnf {
    bool top = false;
    std::vector<Lits> conj;
  };

  explicit Relevance(const Expr &root) : m_ef(root->efac()) {
    // post-order: kids before parents
    std::vector<ENode *> order;
    std::unordered_map<ENode *, bool> seen;
    std::vector<std::pair<ENode *, bool>> st{{root.get(), false}};
    while (!st.empty()) {
      auto [n, exp] = st.back();
      st.pop_back();
      if (exp) {
        order.push_back(n);
        continue;
      }
      if (seen.count(n))
        continue;
      seen[n] = true;
      st.push_back({n, true});
      if (!ackermann_detail::isLeaf(n))
        for (size_t i = 0; i < n->arity(); ++i)
          if (!seen.count(n->arg(i)))
            st.push_back({n->arg(i), false});
    }
    Expr tt = mk<TRUE>(m_ef);
    m_contrib[root.get()].push_back(tt);
    m_cdnf[root.get()].conj.push_back(Lits());
    for (size_t x = order.size(); x-- > 0;) {
      ENode *n = order[x];
      Expr rel = orOf(m_contrib[n]);
      m_contrib.erase(n);
      Dnf d = m_cdnf[n];
      m_cdnf.erase(n);
      m_rel.emplace(n, rel);
      m_dnf.emplace(n, d);
      m_keep.push_back(Expr(n));
      if (ackermann_detail::isLeaf(n))
        continue;
      for (size_t i = 0; i < n->arity(); ++i) {
        ENode *k = n->arg(i);
        if ((isOpX<ITE>(n) && i > 0) || (isOpX<IMPL>(n) && i == 1)) {
          auto l =
              isOpX<ITE>(n) ? mkLit(n->arg(0), i == 1) : mkLit(n->arg(0), true);
          Expr le = litExpr(l);
          m_contrib[k].push_back(isOpX<TRUE>(rel) ? le
                                                  : Expr(mk<AND>(rel, le)));
          addDnf(m_cdnf[k], d, &l);
        } else {
          m_contrib[k].push_back(rel);
          addDnf(m_cdnf[k], d, nullptr);
        }
      }
    }
  }

  /// The relevance term of \p n (null if not in the query).
  Expr rel(ENode *n) const {
    auto it = m_rel.find(n);
    return it == m_rel.end() ? Expr() : it->second;
  }
  const Dnf *dnf(ENode *n) const {
    auto it = m_dnf.find(n);
    return it == m_dnf.end() ? nullptr : &it->second;
  }
  /// Syntactically exclusive: every pair of conjuncts has a contradiction.
  static bool exclusive(const Dnf *a, const Dnf *b) {
    if (!a || !b || a->top || b->top)
      return false;
    Lits m;
    for (const Lits &x : a->conj)
      for (const Lits &y : b->conj)
        if (mergeLits(x, y, m))
          return false;
    return true;
  }

private:
  Expr orOf(std::vector<Expr> &v) {
    for (const Expr &e : v)
      if (isOpX<TRUE>(e))
        return e;
    std::sort(v.begin(), v.end(), [](const Expr &a, const Expr &b) {
      return a->getId() < b->getId();
    });
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v.size() == 1)
      return v[0];
    return mknary<OR>(v.begin(), v.end());
  }
  void addDnf(Dnf &to, const Dnf &from, const std::pair<ENode *, bool> *l) {
    if (to.top)
      return;
    if (from.top) {
      to.top = true;
      to.conj.clear();
      return;
    }
    Lits one, m;
    if (l)
      one.push_back(*l);
    for (const Lits &c : from.conj) {
      if (!mergeLits(c, one, m))
        continue;
      if (std::find(to.conj.begin(), to.conj.end(), m) == to.conj.end())
        to.conj.push_back(m);
      if (to.conj.size() > kDnfCap) {
        to.top = true;
        to.conj.clear();
        return;
      }
    }
  }

  ExprFactory &m_ef;
  std::unordered_map<ENode *, std::vector<Expr>> m_contrib;
  std::unordered_map<ENode *, Dnf> m_cdnf;
  std::unordered_map<ENode *, Expr> m_rel;
  std::unordered_map<ENode *, Dnf> m_dnf;
  std::vector<Expr> m_keep;
};

enum class Mode { Plain, Flat, Arrays, ObjAck, Mux };

struct LowerStats {
  uint64_t arrays = 0, records = 0, extra = 0, unknown = 0, unreached = 0;
  uint64_t pairs = 0, exactDrop = 0, guardDrop = 0, disjointDrop = 0;
  uint64_t arrayPairs = 0, muxPairs = 0, constraints = 0;
  uint64_t constraintsOff = 0, constraintsAddr = 0;
  uint64_t muxObjects = 0, muxRecords = 0, muxCells = 0;
  std::map<std::string, uint64_t> perBucket; ///< records per bucket
  std::map<unsigned, uint64_t> eqWidths;     ///< equalities of the constraints
};

/// One lowered record: its terms in the lowered vocabulary.
struct Lowered {
  Expr addr, off, value, rel;
  long bucket;   ///< -1: unknown
  uint64_t size; ///< mux: the object's size (else 0)
};

/// The encoding of the table (see the header).
class Lowering {
public:
  Lowering(ExprFactory &ef, const BaseRegistry &bases, Translator &tr,
           Mode mode, uint64_t muxMax)
      : m_ef(ef), m_bases(bases), m_tr(tr), m_mode(mode), m_muxMax(muxMax) {}

  Expr lower(const Expr &q) {
    m_rel = std::make_unique<Relevance>(q);
    Ackermannizer probe;
    probe.analyze(q);
    const auto &recs = m_tr.records();
    m_low.assign(recs.size(), Lowered{});
    m_in.assign(recs.size(), false);
    // records of lowerable arrays reached from q
    std::map<ENode *, std::vector<size_t>, IdLess> byArray;
    for (size_t k = 0; k < recs.size(); ++k) {
      if (recs[k].extra) {
        ++m_stats.extra;
        continue;
      }
      if (!m_rel->rel(recs[k].sel.get())) {
        ++m_stats.unreached;
        continue;
      }
      if (probe.arrayIndex(recs[k].array) < 0)
        continue;
      byArray[recs[k].array.get()].push_back(k);
    }
    if (m_mode == Mode::Plain) {
      for (auto &[a, ks] : byArray)
        for (size_t k : ks)
          m_stats.perBucket[bucketName(recs[k])]++;
      return q;
    }
    m_stats.arrays = byArray.size();
    // buckets
    std::map<std::pair<ENode *, ENode *>, long> bucketIds;
    for (auto &[a, ks] : byArray) {
      const bool shadow = m_bases.isShadowRoot(Expr(a));
      for (size_t k : ks) {
        m_in[k] = true;
        m_lowArray.insert(a);
        ++m_stats.records;
        const Record &r = recs[k];
        m_stats.perBucket[bucketName(r)]++;
        long b = -1;
        if (!r.leaf.base)
          ++m_stats.unknown;
        if (m_mode == Mode::Flat || shadow)
          b = bucketIds
                  .emplace(std::make_pair(a, (ENode *)nullptr),
                           bucketIds.size())
                  .first->second;
        else if (r.leaf.base)
          b = bucketIds
                  .emplace(std::make_pair(a, r.leaf.base.get()),
                           bucketIds.size())
                  .first->second;
        m_low[k].bucket = b;
        m_flat[k] = (m_mode == Mode::Flat || shadow);
      }
    }
    // mux objects and per-object arrays
    if (m_mode == Mode::Mux)
      for (auto &[key, b] : bucketIds) {
        if (!key.second)
          continue;
        const ObjectBase *o = m_bases.lookup(Expr(key.second));
        if (o && o->size.getBitWidth() <= 64 && !o->size.isZero() &&
            o->size.getZExtValue() <= m_muxMax) {
          m_muxSize[b] = o->size.getZExtValue();
          ++m_stats.muxObjects;
          m_stats.muxCells += o->size.getZExtValue();
        }
      }
    // the substitution: record selects -> values (bottom-up, so loaded
    // addresses are lowered first)
    Expr body = lowerTerm(q);
    // constraints
    std::vector<Expr> conj{body};
    for (auto &[a, ks] : byArray) {
      for (size_t x = 0; x < ks.size(); ++x)
        for (size_t y = x + 1; y < ks.size(); ++y)
          pair(ks[x], ks[y], conj);
    }
    // lowered relevance of each record (for the model and the checks)
    for (size_t k = 0; k < recs.size(); ++k)
      if (m_in[k])
        m_low[k].rel = lowerTerm(m_rel->rel(recs[k].sel.get()));
    return conj.size() == 1 ? body : mknary<AND>(conj.begin(), conj.end());
  }

  /// \p e in the lowered vocabulary (records of lowered arrays replaced).
  Expr lowerTerm(const Expr &e) {
    return ackermann_detail::rewriteDag(
        e, m_memo, m_keep, [](ENode *n) { return Expr(n); },
        [&](ENode *n, llvm::SmallVectorImpl<Expr> &kids) -> Expr {
          if (isOpX<SELECT>(n)) {
            long k = m_tr.recordOf(n);
            if (k >= 0 && lowered(size_t(k)))
              return value(size_t(k), kids[1]);
          }
          return ackermann_detail::withKids(n, kids);
        });
  }

  const LowerStats &stats() const { return m_stats; }
  const Relevance &relevance() const { return *m_rel; }
  bool lowered(size_t k) const { return k < m_in.size() && m_in[k]; }
  const Lowered &low(size_t k) const { return m_low[k]; }
  bool isLoweredArray(ENode *a) const { return m_lowArray.count(a); }
  /// Omitted pairs (different object buckets), in order.
  const std::vector<std::pair<size_t, size_t>> &omitted() const {
    return m_omitted;
  }
  /// Records read through a mux: their out-of-range constant is free.
  const std::vector<size_t> &muxRecords() const { return m_muxRecs; }

private:
  static std::string bucketName(const Record &r) {
    std::string s;
    llvm::raw_string_ostream os(s);
    os << *op::bind::fname(r.array->first()) << "/";
    if (r.leaf.base)
      os << *op::bind::fname(r.leaf.base->first());
    else
      os << "unknown";
    return os.str();
  }

  Expr fresh(const std::string &name, const Expr &sort) {
    return bind::mkConst(mkTerm<std::string>(name, m_ef), sort);
  }

  Expr cellSort(size_t k) {
    return sort::arrayValTy(op::bind::declaredSort(m_tr.records()[k].array));
  }

  /// The value of record \p k (\p addr: its lowered address).
  Expr value(size_t k, const Expr &addr) {
    Lowered &L = m_low[k];
    if (L.value)
      return L.value;
    const Record &r = m_tr.records()[k];
    L.addr = addr;
    L.off = r.leaf.base ? lowerTerm(r.leaf.offE) : addr;
    const std::string id = std::to_string(k);
    auto mux = m_muxSize.find(L.bucket);
    if (m_mode == Mode::Mux && L.bucket >= 0 && !m_flat[k] &&
        mux != m_muxSize.end()) {
      uint64_t size = mux->second;
      L.size = size;
      m_muxRecs.push_back(k);
      ++m_stats.muxRecords;
      auto &cells = m_cells[L.bucket];
      if (cells.empty())
        for (uint64_t c = 0; c < size; ++c)
          cells.push_back(fresh("xrt.cell." + std::to_string(L.bucket) + "." +
                                    std::to_string(c),
                                cellSort(k)));
      unsigned bits = 0;
      while ((uint64_t(1) << bits) < size)
        ++bits;
      unsigned w =
          op::bv::width(sort::arrayIndexTy(op::bind::declaredSort(r.array)));
      Expr m = muxTree(cells, L.off, bits, 0, 0);
      Expr in = mk<BULT>(
          L.off, op::bv::bvnum(mpz_class((unsigned long)size), w, m_ef));
      L.value = mk<ITE>(in, m, fresh("xrt.out." + id, cellSort(k)));
      m_stats.eqWidths[1] += bits;
    } else if (m_mode == Mode::Arrays && L.bucket >= 0 && !m_flat[k]) {
      auto &arr = m_objArrays[L.bucket];
      if (!arr) {
        std::string s;
        llvm::raw_string_ostream os(s);
        os << "xrt." << *op::bind::fname(r.array->first()) << "."
           << *op::bind::fname(r.leaf.base->first());
        arr = fresh(os.str(), op::bind::declaredSort(r.array));
      }
      L.value = op::array::select(arr, L.off);
    } else {
      L.value = fresh("xrt.r." + id, cellSort(k));
    }
    return L.value;
  }

  Expr muxTree(const std::vector<Expr> &cells, const Expr &off, unsigned bits,
               unsigned level, uint64_t prefix) {
    if (level == bits)
      return cells[std::min<uint64_t>(prefix, cells.size() - 1)];
    unsigned bit = bits - 1 - level;
    Expr hi =
        muxTree(cells, off, bits, level + 1, prefix | (uint64_t(1) << bit));
    Expr lo = muxTree(cells, off, bits, level + 1, prefix);
    if (hi == lo)
      return hi;
    Expr b = mk<EQ>(op::bv::extract(bit, bit, off),
                    op::bv::bvnum(mpz_class(1UL), 1, m_ef));
    return mk<ITE>(b, hi, lo);
  }

  void pair(size_t k, size_t l, std::vector<Expr> &conj) {
    const auto &recs = m_tr.records();
    const Record &a = recs[k], &b = recs[l];
    ++m_stats.pairs;
    const Lowered &A = m_low[k], &B = m_low[l];
    // values are built on first use; a record whose select was not reached
    // by lowerTerm (cannot happen for reached ones) gets one here
    if (!A.value)
      value(k, lowerTerm(a.leaf.addr));
    if (!B.value)
      value(l, lowerTerm(b.leaf.addr));
    const bool sameBucket = A.bucket >= 0 && A.bucket == B.bucket;
    const bool useOff = sameBucket && !m_flat[k] && a.leaf.base && b.leaf.base;
    // exact: same symbolic part, different constants
    const Lin &la = useOff ? a.leaf.off : a.leaf.full;
    const Lin &lb = useOff ? b.leaf.off : b.leaf.full;
    if (la.coef == lb.coef && la.c != lb.c) {
      ++m_stats.exactDrop;
      return;
    }
    if (Relevance::exclusive(m_rel->dnf(a.sel.get()),
                             m_rel->dnf(b.sel.get()))) {
      ++m_stats.guardDrop;
      return;
    }
    if (A.bucket >= 0 && B.bucket >= 0 && A.bucket != B.bucket) {
      ++m_stats.disjointDrop;
      m_omitted.emplace_back(k, l);
      return;
    }
    if (sameBucket && m_mode == Mode::Arrays && !m_flat[k]) {
      ++m_stats.arrayPairs;
      return;
    }
    if (sameBucket && m_mode == Mode::Mux && !m_flat[k] &&
        m_muxSize.count(A.bucket)) {
      ++m_stats.muxPairs;
      return;
    }
    ++m_stats.constraints;
    Expr x = useOff ? A.off : A.addr, y = useOff ? B.off : B.addr;
    ++(useOff ? m_stats.constraintsOff : m_stats.constraintsAddr);
    TypeChecker tc;
    m_stats.eqWidths[op::bv::width(tc.typeOf(x))]++;
    conj.push_back(mk<OR>(mk<NEG>(mk<EQ>(x, y)), mk<EQ>(A.value, B.value)));
  }

  ExprFactory &m_ef;
  const BaseRegistry &m_bases;
  Translator &m_tr;
  Mode m_mode;
  uint64_t m_muxMax;
  std::unique_ptr<Relevance> m_rel;
  std::vector<Lowered> m_low;
  std::vector<bool> m_in;
  std::unordered_map<size_t, bool> m_flat;
  std::unordered_set<ENode *> m_lowArray;
  std::map<long, uint64_t> m_muxSize;
  std::map<long, std::vector<Expr>> m_cells;
  std::map<long, Expr> m_objArrays;
  std::vector<std::pair<size_t, size_t>> m_omitted;
  std::vector<size_t> m_muxRecs;
  std::unordered_map<ENode *, Expr> m_memo;
  std::vector<Expr> m_keep;
  LowerStats m_stats;
};

/// A model of the original query from a model of the lowered one: each
/// lowered array is the finite function of its records, relevant records
/// first (their relevance true in the model); irrelevant records only fill
/// addresses no relevant record has. Reads like AckermannModel.
class TableModel : public sea::solver::Model {
public:
  TableModel(sea::solver::Model &inner, const Translator &tr,
             const Lowering &lw)
      : m_inner(inner), m_tr(tr), m_lw(lw) {}

  uint64_t inconsistentRelevant() const { return m_incRel; }
  uint64_t inconsistentIrrelevant() const { return m_incIrr; }
  uint64_t relevant() const { return m_relevant; }
  uint64_t defaulted() const { return m_defaulted; }
  uint64_t broken() const { return m_brokenRecs; }

  Expr eval(Expr e, bool complete) override {
    using namespace ackermann_detail;
    if (m_broken)
      return Expr();
    Expr t = rewriteDag(
        e, m_memo, m_keep, [](ENode *n) { return Expr(n); },
        [&](ENode *n, llvm::SmallVectorImpl<Expr> &kids) -> Expr {
          if (m_broken || !isOpX<SELECT>(n) || !hasLowered(n->arg(0)))
            return withKids(n, kids);
          Expr j = m_inner.eval(kids[1], /*complete=*/true);
          if (!j || !op::bv::is_bvnum(j)) {
            m_broken = true;
            return withKids(n, kids);
          }
          Expr v =
              readMem(Expr(n->arg(0)), reducedValue(j), op::bv::widthBvNum(j));
          return v ? v : withKids(n, kids);
        });
    if (m_broken)
      return Expr();
    if (hasLowered(t.get()))
      t = rewriteDag(
          t, m_asArray, m_keep,
          [&](ENode *n) -> Expr {
            return isArrayConstant(n) && m_lw.isLoweredArray(n) ? asArray(n)
                                                                : Expr(n);
          },
          [](ENode *n, llvm::SmallVectorImpl<Expr> &kids) {
            return withKids(n, kids);
          });
    return m_inner.eval(t, complete);
  }

  void print(llvm::raw_ostream &o) const override { m_inner.print(o); }

  /// A lowered array's cell is a default iff no record has its address
  /// (readMem gives 0 there); other arrays: the inner model's answer.
  bool defaultCell(Expr arr, Expr idx) override {
    using namespace ackermann_detail;
    if (!arr || !isArrayConstant(arr.get()) || !m_lw.isLoweredArray(arr.get()))
      return m_inner.defaultCell(arr, idx);
    if (!idx || !op::bv::is_bvnum(idx))
      return false;
    return !function(arr.get()).cells.count(reducedValue(idx));
  }

private:
  struct Fn {
    std::map<mpz_class, Expr> cells;
    bool built = false;
  };

  Fn &function(ENode *a) {
    using namespace ackermann_detail;
    Fn &f = m_funcs[a];
    if (f.built)
      return f;
    f.built = true;
    const auto &recs = m_tr.records();
    std::map<mpz_class, Expr> irr;
    for (size_t k = 0; k < recs.size(); ++k) {
      if (recs[k].array.get() != a || !m_lw.lowered(k))
        continue;
      const Lowered &L = m_lw.low(k);
      Expr i = L.addr ? m_inner.eval(L.addr, true) : Expr();
      Expr v = L.value ? m_inner.eval(L.value, true) : Expr();
      Expr r = L.rel ? m_inner.eval(L.rel, true) : Expr();
      if (!i || !v || !r || !op::bv::is_bvnum(i) || !op::bv::is_bvnum(v)) {
        ++m_brokenRecs;
        continue;
      }
      Expr vn =
          op::bv::bvnum(reducedValue(v), op::bv::widthBvNum(v), v->efac());
      bool rel = !isOpX<FALSE>(r);
      if (getenv("XRT_DEBUG"))
        llvm::errs() << "xrt.model-record: " << k << " addr=" << *i
                     << " value=" << *v << " rel=" << *r << "\n";
      auto &tbl = rel ? f.cells : irr;
      if (rel)
        ++m_relevant;
      auto [it, fresh] = tbl.emplace(reducedValue(i), vn);
      if (!fresh && it->second != vn)
        ++(rel ? m_incRel : m_incIrr);
    }
    for (auto &[i, v] : irr)
      f.cells.emplace(i, v);
    return f;
  }

  bool hasLowered(ENode *root) {
    using namespace ackermann_detail;
    std::vector<std::pair<ENode *, bool>> stack{{root, false}};
    while (!stack.empty()) {
      auto [n, expanded] = stack.back();
      if (m_has.count(n)) {
        stack.pop_back();
        continue;
      }
      if (isLeaf(n)) {
        stack.pop_back();
        m_has.emplace(n, isArrayConstant(n) && m_lw.isLoweredArray(n));
        m_keep.push_back(Expr(n));
        continue;
      }
      if (!expanded) {
        stack.back().second = true;
        for (size_t i = 0; i < n->arity(); ++i)
          if (!m_has.count(n->arg(i)))
            stack.push_back({n->arg(i), false});
        continue;
      }
      stack.pop_back();
      bool h = false;
      for (size_t i = 0; i < n->arity() && !h; ++i)
        h = m_has.find(n->arg(i))->second;
      m_has.emplace(n, h);
      m_keep.push_back(Expr(n));
    }
    return m_has.find(root)->second;
  }

  Expr zero(ENode *a) {
    unsigned w =
        op::bv::width(sort::arrayValTy(op::bind::declaredSort(Expr(a))));
    return op::bv::bvnum(mpz_class(0UL), w, a->efac());
  }

  Expr readMem(Expr mem, const mpz_class &idx, unsigned w) {
    using namespace ackermann_detail;
    while (true) {
      if (!hasLowered(mem.get()))
        return m_inner.eval(
            op::array::select(mem, op::bv::bvnum(idx, w, mem->efac())), true);
      if (isArrayConstant(mem.get()) && m_lw.isLoweredArray(mem.get())) {
        Fn &f = function(mem.get());
        auto it = f.cells.find(idx);
        if (it != f.cells.end())
          return it->second;
        ++m_defaulted;
        return zero(mem.get());
      }
      if (isOpX<STORE>(mem)) {
        Expr i = eval(mem->arg(1), true);
        if (!i || !op::bv::is_bvnum(i))
          return Expr();
        if (reducedValue(i) == idx)
          return eval(mem->arg(2), true);
        mem = mem->arg(0);
        continue;
      }
      if (isOpX<ITE>(mem)) {
        Expr c = eval(mem->arg(0), true);
        if (!c || !(isOpX<TRUE>(c) || isOpX<FALSE>(c)))
          return Expr();
        mem = isOpX<TRUE>(c) ? mem->arg(1) : mem->arg(2);
        continue;
      }
      return Expr();
    }
  }

  Expr asArray(ENode *a) {
    Fn &f = function(a);
    Expr sort = op::bind::declaredSort(Expr(a));
    Expr isort = sort::arrayIndexTy(sort);
    unsigned iw = op::bv::width(isort);
    Expr res = op::array::constArray(isort, zero(a));
    for (const auto &[i, v] : f.cells)
      res = op::array::store(res, op::bv::bvnum(i, iw, a->efac()), v);
    return res;
  }

  sea::solver::Model &m_inner;
  const Translator &m_tr;
  const Lowering &m_lw;
  std::map<ENode *, Fn> m_funcs;
  std::unordered_map<ENode *, Expr> m_memo, m_asArray;
  std::unordered_map<ENode *, bool> m_has;
  std::vector<Expr> m_keep;
  bool m_broken = false;
  uint64_t m_incRel = 0, m_incIrr = 0, m_relevant = 0, m_defaulted = 0,
           m_brokenRecs = 0;
};

} // namespace xrt
} // namespace expr
