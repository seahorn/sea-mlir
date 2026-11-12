// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// Memory-operation terms (docs/design/SeaM-memops.md).
//
// The terms (operators in ExprOpArray.hh), for byte (index-unit) memories:
//
//   mcopy(Md, Ms, d, s, n)   cell j:  in(j, d, n) ? Ms[s + (j - d)] : Md[j]
//   mset(M, d, n, v)         cell j:  in(j, d, n) ? v : M[j]
//   mhavoc(M, H, d, n)       cell j:  in(j, d, n) ? H[j] : M[j]
//
//   in(j, d, n) := (j - d) <u n        (modulo 2^w: exact for n = 0 and for a
//                                       range that wraps around 2^w)
//
// Ms is the memory version a memmove reads (its source operand), so an
// overlapping copy is exact. H is the fresh array of a memhavoc (nd.<site>),
// one per op: every read of the term reads the same H.
//
// Like store-map (StoreMap.hh) the terms are internal: memory values of the VC
// hold them, no solver knows them. Two things live here.
//
//  The defining read equations (memopRead): select(T, j) as an ite over the
//  memory below the term. Used by the optimizing rules (SeaMRules.hh) for the
//  undecided case and by the boundary.
//
//  MemOpTranslator: the solver boundary. A semantics-only translation,
//  defined in SeaM.md's "Meaning of SeaM terms": every term is eliminated
//  by its defining read equation, nothing else. Each select is pushed down
//  through the memories that contain a term below them:
//    select(store(M, i, v), j)       = ite(i = j, v, select(M, j))
//    select(ite(c, M1, M2), j)       = ite(c, select(M1, j), select(M2, j))
//    select(mcopy(Md, Ms, d, s, n), j)
//                                    = ite((j - d) <u n, select(Ms, s + (j -
//                                    d)),
//                                          select(Md, j))
//    select(mset(M, d, n, v), j)     = ite((j - d) <u n, v, select(M, j))
//    select(mhavoc(M, H, d, n), j)   = ite((j - d) <u n, select(H, j),
//    select(M, j))
//  and stops at a memory with no term below it (select(M, j) is kept: the
//  solver's array theory). No assumption (no A-INB), no address reasoning (no
//  ARM, no compare decided), no dependence on what the rewriter did: a
//  residual select over a term (--seam-row-expand=0, a rewrite stopped at its
//  call limit) is an ordinary case. Memoized by node for terms and by
//  (memory, index) for reads, iterative (VC memories are deep chains).
//  A memory that holds a term in a position that is not read (an equality of
//  memories, a memory-valued root) has no translation without lambdas: it is
//  refused (translate() returns null and says why), by design. Store-maps
//  must be expanded before (SeaMRewriter::solverTerm does both, store-maps
//  first).
#include "sea/Expr/Expr.hh"
#include "sea/Expr/TypeChecker.hh"

#include "llvm/ADT/SmallVector.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace expr {

/// One of mcopy / mset / mhavoc.
inline bool isMemOpTerm(const Expr &e) {
  return isOpX<MEM_COPY>(e) || isOpX<MEM_SET>(e) || isOpX<MEM_HAVOC>(e);
}

/// The memory a term writes (the result outside the range).
inline Expr memOpBase(const Expr &t) { return t->arg(0); }
/// The destination address d of a term.
inline Expr memOpDst(const Expr &t) {
  return t->arg(isOpX<MEM_SET>(t) ? 1 : 2);
}
/// The length n of a term (index units).
inline Expr memOpLen(const Expr &t) {
  return isOpX<MEM_COPY>(t)  ? t->arg(4)
         : isOpX<MEM_SET>(t) ? t->arg(2)
                             : t->arg(3);
}

/// The membership test in(j, d, n) = (j - d) <u n, given k = j - d.
inline Expr memOpIn(const Expr &k, const Expr &n) { return mk<BULT>(k, n); }

/// The hit form of a read of term \p t at \p j, with k = j - d: the source
/// read select(Ms, s + k) for mcopy, v for mset, select(H, j) for mhavoc.
inline Expr memOpHit(const Expr &t, const Expr &j, const Expr &k) {
  if (isOpX<MEM_COPY>(t))
    return op::array::select(t->arg(1), op::ptr::addressPlus(t->arg(3), k));
  if (isOpX<MEM_SET>(t))
    return t->arg(3);
  return op::array::select(t->arg(1), j);
}

/// The defining read equation of term \p t at \p j (the header), one level:
/// ite((j - d) <u n, <hit>, select(base, j)).
inline Expr memopRead(const Expr &t, const Expr &j) {
  Expr k = mk<PTR_DIFF>(j, memOpDst(t));
  return mk<ITE>(memOpIn(k, memOpLen(t)), memOpHit(t, j, k),
                 op::array::select(memOpBase(t), j));
}

/// Is \p e a memory (an array value) by its head: a store, a store-map, a
/// term, a constant array, an ite of memories, an array-sorted constant?
inline bool isMemoryHead(Expr e) {
  while (isOpX<ITE>(e))
    e = e->arg(1);
  if (isOpX<STORE>(e) || isOpX<STORE_MAP>(e) || isMemOpTerm(e) ||
      isOpX<CONST_ARRAY>(e) || isOpX<W_STORE>(e))
    return true;
  // a constant of an array sort (its declared sort, no inference)
  return op::bind::isArrayConst(e);
}

/// The boundary expansion of width terms (docs/design/SeaM-mixed-width.md):
///   wstore(M, a, v)  ~>  store(... store(M, a, v[0..8)) ..., a + w-1,
///                                  v[8(w-1)..8w))
///   wselect(M, a, S) ~>  concat(select(M, a + r-1), ..., select(M, a))
/// little endian, addresses modulo the index width. Semantics only (the
/// definition of the terms); memoized by node, iterative. Run before the
/// memory-operation translation (the byte reads it makes may be over memop
/// terms).
class WideExpander {
public:
  Expr expand(const Expr &root) {
    if (!hasWide(root))
      return root;
    std::vector<std::pair<Expr, bool>> stack{{root, false}};
    while (!stack.empty()) {
      auto [e, expanded] = stack.back();
      stack.pop_back();
      if (m_memo.count(e.get()))
        continue;
      if (!expanded && e->arity() > 0) {
        stack.push_back({e, true});
        for (size_t i = 0; i < e->arity(); ++i)
          if (!m_memo.count(e->arg(i)))
            stack.push_back({Expr(e->arg(i)), false});
        continue;
      }
      ExprVector kids;
      bool changed = false;
      for (size_t i = 0; i < e->arity(); ++i) {
        Expr k = m_memo.at(e->arg(i));
        changed |= k.get() != e->arg(i);
        kids.push_back(k);
      }
      Expr res = changed ? e->efac().mkNary(e->op(), kids) : e;
      if (isOpX<W_STORE>(res))
        res = store(res->arg(0), res->arg(1), res->arg(2));
      else if (isOpX<W_SELECT>(res) &&
               !(m_keepOverArrayConstants && isArrayConstant(res->arg(0))))
        res = select(res->arg(0), res->arg(1), res->arg(2));
      m_keep.push_back(res);
      m_memo.emplace(e.get(), res);
      m_keep.push_back(e);
    }
    return m_memo.at(root.get());
  }

  /// Does \p e contain a width term? (memoized)
  bool hasWide(const Expr &e) {
    auto it = m_has.find(e.get());
    if (it != m_has.end())
      return it->second;
    std::vector<std::pair<ENode *, bool>> stack{{e.get(), false}};
    while (!stack.empty()) {
      auto [n, done] = stack.back();
      stack.pop_back();
      if (m_has.count(n))
        continue;
      if (isOpX<W_STORE>(n) || isOpX<W_SELECT>(n)) {
        m_has[n] = true;
        m_pinned.push_back(Expr(n));
        continue;
      }
      if (!done) {
        stack.push_back({n, true});
        for (size_t i = 0; i < n->arity(); ++i)
          if (!m_has.count(n->arg(i)))
            stack.push_back({n->arg(i), false});
        continue;
      }
      bool any = false;
      for (size_t i = 0; i < n->arity() && !any; ++i)
        any = m_has.at(n->arg(i));
      m_has[n] = any;
      // the cache is keyed by node address: pin every key (a negative
      // entry for a node that is later released would answer for a new
      // node at the same address)
      m_pinned.push_back(Expr(n));
    }
    return m_has.at(e.get());
  }

  uint64_t expanded() const { return m_expanded; }

  /// Leave a wselect directly over an array constant (an initial memory or a
  /// memhavoc array) for the initial-words step (InitialWords.hh; its byte
  /// expansion comes after it). Set before the first expand().
  void keepOverArrayConstants(bool b) { m_keepOverArrayConstants = b; }
  /// Byte k of an access at an ite address with k pushed into the leaves
  /// (op::ptr::byteAddress) when the ite has at most \p n leaves; 0: a + k
  /// over the ite (as before). Set before the first expand().
  void maxOffsetLeaves(unsigned n) { m_maxOffsetLeaves = n; }
  /// addresses built with the offset pushed into ite leaves / refused (over
  /// the leaf cap)
  uint64_t offsetPushed() const { return m_offsetPushed; }
  uint64_t offsetRefused() const { return m_offsetRefused; }

private:
  static bool isArrayConstant(ENode *n) {
    return op::bind::isArrayConst(Expr(n));
  }

  /// a + k at the address width of a; a K-node when a is a promised node
  /// (op::ptr::addressPlus); k pushed into the leaves of an ite a
  /// (op::ptr::byteAddress, at most m_maxOffsetLeaves leaves)
  Expr at(const Expr &a, unsigned k) {
    if (k == 0)
      return a;
    unsigned w = op::ptr::addrWidth(a, m_tc);
    // a node (PtrSort rev 5, S1.3): expanded at the boundary to its flat form
    op::ptr::OffsetPush how;
    Expr res = op::ptr::byteAddress(
        a, op::bv::bvnum(mpz_class(k), w, a->efac()), m_maxOffsetLeaves, how);
    m_offsetPushed += how == op::ptr::OffsetPush::Pushed;
    m_offsetRefused += how == op::ptr::OffsetPush::Refused;
    return res;
  }
  Expr store(const Expr &m, const Expr &a, const Expr &v) {
    ++m_expanded;
    unsigned bytes = op::bv::width(m_tc.typeOf(v)) / 8;
    Expr res = m;
    for (unsigned k = 0; k < bytes; ++k)
      res =
          op::array::store(res, at(a, k), op::bv::extract(8 * k + 7, 8 * k, v));
    return res;
  }
  Expr select(const Expr &m, const Expr &a, const Expr &sort) {
    ++m_expanded;
    unsigned bytes = op::bv::width(sort) / 8;
    Expr res = op::array::select(m, at(a, 0));
    for (unsigned k = 1; k < bytes; ++k)
      res = op::bv::concat(op::array::select(m, at(a, k)), res);
    return res;
  }

  TypeChecker m_tc;
  std::unordered_map<ENode *, Expr> m_memo;
  std::unordered_map<ENode *, bool> m_has;
  std::vector<Expr> m_keep;
  std::vector<Expr> m_pinned; ///< the keys of m_has
  uint64_t m_expanded = 0;
  uint64_t m_offsetPushed = 0, m_offsetRefused = 0;
  unsigned m_maxOffsetLeaves = 0;
  bool m_keepOverArrayConstants = false;
};

/// The boundary translation (see the header). One instance keeps its memo for
/// the VC: destroy it before the ExprFactory.
class MemOpTranslator {
public:
  /// \p root with every term eliminated by its read equations, or null if a
  /// term is in a position that is not read (\p why says where). A term-free
  /// \p root is returned as it is.
  Expr translate(const Expr &root, std::string &why) {
    if (!hasTerm(root))
      return root;
    std::vector<Frame> stack;
    stack.push_back(Frame{false, root, Expr(), 0});
    while (!stack.empty()) {
      if (!step(stack, why))
        return Expr();
    }
    return m_memoT.find(root.get())->second;
  }

  /// reads over a term translated so far (a select(term, j) eliminated by its
  /// read equation): 0 unless a term reached the boundary
  uint64_t translated() const { return m_translated; }

  /// Does \p e contain a memory-operation term? Memoized, iterative.
  bool hasTerm(const Expr &e) {
    auto it = m_has.find(e.get());
    if (it != m_has.end())
      return it->second;
    std::vector<std::pair<ENode *, bool>> stack{{e.get(), false}};
    while (!stack.empty()) {
      auto [n, expanded] = stack.back();
      stack.pop_back();
      if (m_has.count(n))
        continue;
      if (n->arity() == 0 || isOpX<BIND>(n) ||
          (isOpX<FAPP>(n) && n->arity() == 1)) {
        m_has.emplace(n, false);
        m_keep.push_back(Expr(n));
        continue;
      }
      if (!expanded) {
        stack.push_back({n, true});
        for (size_t i = 0; i < n->arity(); ++i)
          if (!m_has.count(n->arg(i)))
            stack.push_back({n->arg(i), false});
        continue;
      }
      bool h = isMemOpTerm(Expr(n));
      for (size_t i = 0; i < n->arity() && !h; ++i)
        h = m_has.find(n->arg(i))->second;
      m_has.emplace(n, h);
      m_keep.push_back(Expr(n));
    }
    return m_has.find(e.get())->second;
  }

private:
  /// T(e): translate a term; R(m, j): read memory m at the translated index j.
  struct Frame {
    bool read;
    Expr e; // the term (T) or the memory (R)
    Expr j; // R: the index (already translated)
    unsigned stage;
  };
  struct ReadKey {
    ENode *m, *j;
    bool operator==(const ReadKey &o) const { return m == o.m && j == o.j; }
  };
  struct ReadKeyHash {
    size_t operator()(const ReadKey &k) const {
      return std::hash<ENode *>()(k.m) * 31 + std::hash<ENode *>()(k.j);
    }
  };
  struct ReadVal {
    Expr m, j, res; // m, j keep the key alive
  };

  const Expr *doneT(const Expr &e) {
    auto it = m_memoT.find(e.get());
    return it == m_memoT.end() ? nullptr : &it->second;
  }
  const Expr *doneR(const Expr &m, const Expr &j) {
    auto it = m_memoR.find(ReadKey{m.get(), j.get()});
    return it == m_memoR.end() ? nullptr : &it->second.res;
  }
  void setT(const Expr &e, const Expr &v) {
    m_memoT.emplace(e.get(), v);
    m_keep.push_back(e);
  }
  void setR(const Expr &m, const Expr &j, const Expr &v) {
    m_memoR.emplace(ReadKey{m.get(), j.get()}, ReadVal{m, j, v});
  }

  /// Pushes T(e) if not done; true if it was done.
  bool needT(std::vector<Frame> &st, const Expr &e) {
    if (doneT(e))
      return true;
    if (!hasTerm(e)) {
      setT(e, e);
      return true;
    }
    st.push_back(Frame{false, e, Expr(), 0});
    return false;
  }
  bool needR(std::vector<Frame> &st, const Expr &m, const Expr &j) {
    if (doneR(m, j))
      return true;
    if (!hasTerm(m)) {
      setR(m, j, op::array::select(m, j));
      return true;
    }
    st.push_back(Frame{true, m, j, 0});
    return false;
  }
  Expr t(const Expr &e) { return *doneT(e); }
  Expr r(const Expr &m, const Expr &j) { return *doneR(m, j); }

  /// One step on the top frame; false on a refusal.
  bool step(std::vector<Frame> &st, std::string &why) {
    size_t top = st.size() - 1;
    Frame f = st[top];
    if (!f.read) {
      if (doneT(f.e)) {
        st.pop_back();
        return true;
      }
      const Expr &e = f.e;
      if (isOpX<SELECT>(e)) {
        if (f.stage == 0) {
          st[top].stage = 1;
          needT(st, e->arg(1));
          return true;
        }
        Expr j = t(e->arg(1));
        if (f.stage == 1) {
          st[top].stage = 2;
          needR(st, e->arg(0), j);
          return true;
        }
        setT(e, r(e->arg(0), j));
        st.pop_back();
        return true;
      }
      if (isMemoryHead(e)) {
        why = std::string("a memory holding a memory-operation term (") +
              e->op().name() + ") in a position that is not read" +
              (st.size() > 1 ? std::string(" (a kid of ") +
                                   st[top - 1].e->op().name() + ")"
                             : std::string(" (the root)"));
        return false;
      }
      if (f.stage == 0) {
        st[top].stage = 1;
        for (size_t i = e->arity(); i-- > 0;)
          needT(st, e->arg(i));
        return true;
      }
      llvm::SmallVector<Expr, 4> kids;
      bool changed = false;
      for (size_t i = 0; i < e->arity(); ++i) {
        Expr k = t(e->arg(i));
        changed = changed || k.get() != e->arg(i);
        kids.push_back(k);
      }
      setT(e, changed
                  ? e->getFactory().mkNary(e->op(), kids.begin(), kids.end())
                  : e);
      st.pop_back();
      return true;
    }
    // a read of memory m at the translated index j
    const Expr m = f.e, j = f.j;
    if (doneR(m, j)) {
      st.pop_back();
      return true;
    }
    if (isOpX<STORE>(m)) {
      if (f.stage == 0) {
        st[top].stage = 1;
        needT(st, m->arg(1));
        needT(st, m->arg(2));
        return true;
      }
      if (f.stage == 1) {
        st[top].stage = 2;
        needR(st, m->arg(0), j);
        return true;
      }
      setR(m, j,
           mk<ITE>(mk<EQ>(t(m->arg(1)), j), t(m->arg(2)), r(m->arg(0), j)));
      st.pop_back();
      return true;
    }
    if (isOpX<ITE>(m)) {
      if (f.stage == 0) {
        st[top].stage = 1;
        needT(st, m->arg(0));
        return true;
      }
      if (f.stage == 1) {
        st[top].stage = 2;
        needR(st, m->arg(1), j);
        needR(st, m->arg(2), j);
        return true;
      }
      setR(m, j, mk<ITE>(t(m->arg(0)), r(m->arg(1), j), r(m->arg(2), j)));
      st.pop_back();
      return true;
    }
    if (isMemOpTerm(m)) {
      // the operands that are values: d, n, and s (mcopy) / v (mset)
      const Expr d = memOpDst(m), n = memOpLen(m);
      if (f.stage == 0) {
        st[top].stage = 1;
        needT(st, d);
        needT(st, n);
        if (isOpX<MEM_COPY>(m))
          needT(st, m->arg(3));
        else if (isOpX<MEM_SET>(m))
          needT(st, m->arg(3));
        return true;
      }
      Expr k = mk<PTR_DIFF>(j, t(d)); // erased with the nodes after
      Expr src;                       // mcopy: the translated source index
      if (isOpX<MEM_COPY>(m))
        src = op::ptr::addressPlus(t(m->arg(3)), k);
      if (f.stage == 1) {
        st[top].stage = 2;
        needR(st, m->arg(0), j);
        if (isOpX<MEM_COPY>(m))
          needR(st, m->arg(1), src);
        else if (isOpX<MEM_HAVOC>(m))
          needR(st, m->arg(1), j);
        return true;
      }
      Expr hit = isOpX<MEM_COPY>(m)  ? r(m->arg(1), src)
                 : isOpX<MEM_SET>(m) ? t(m->arg(3))
                                     : r(m->arg(1), j);
      ++m_translated;
      setR(m, j, mk<ITE>(memOpIn(k, t(n)), hit, r(m->arg(0), j)));
      st.pop_back();
      return true;
    }
    if (isOpX<CONST_ARRAY>(m)) {
      // a constant memory whose value holds a term (`sea.mem.const %x` with
      // %x read through a memop): every cell is the value.
      if (f.stage == 0) {
        st[top].stage = 1;
        needT(st, m->arg(1));
        return true;
      }
      setR(m, j, t(m->arg(1)));
      st.pop_back();
      return true;
    }
    why = std::string("a read of a memory '") + m->op().name() +
          "' that holds a memory-operation term the translation does not "
          "know";
    return false;
  }

  std::unordered_map<ENode *, bool> m_has;
  std::unordered_map<ENode *, Expr> m_memoT;
  std::unordered_map<ReadKey, ReadVal, ReadKeyHash> m_memoR;
  /// keeps the keys of m_has / m_memoT alive
  std::vector<Expr> m_keep;
  uint64_t m_translated = 0;
};

} // end of namespace expr
