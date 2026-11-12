// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/// Pointer arithmetic as pointer arithmetic: the PTR_INDEX operators
/// (docs/design/PtrIndex.md, rev 7, section 2).
///
///   PTR_INDEX*(b, x_1, s_1, ..., x_m, s_m, c, K)
///
/// - b: the base (an n-bit address term);
/// - x_k: an index as an n-bit term (the VC applies the sign extension);
/// - s_k, c, K: Terminal<mpz_class> integers: the strides, the constant
///   offset, and the byte offset K of an access inside the expansion of a
///   wider one (0 from the VC; only the expanders write K != 0).
/// Value: b + sum x_k*s_k + c + K (mod 2^n). The operator carries the
/// front end's promise (a trusted fact, about b + sum x_k*s_k + c):
///   PTR_INDEX       none
///   PTR_INDEX_NUSW  inbounds / nusw: products, total offset (signed) and
///                   b + offset fit (P1-P3)
///   PTR_INDEX_NUW   the unsigned forms
/// The flags are part of the term's identity (hash-consing never merges a
/// promise into a term without one). No solver knows these operators:
/// PtrIndexExpander turns them into bit-vector arithmetic before any
/// backend (the boundary checks: vc.ptr-sort-at-boundary).
#pragma once
#include "sea/Expr/ExprApi.hh"
#include "sea/Expr/ExprCore.hh"
#include "sea/Expr/ExprOpBv.hh"
#include "sea/Expr/ExprOpCore.hh"
#include "sea/Expr/TypeChecker.hh"

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace expr {
namespace op {
enum class PtrOpKind {
  PTR_INDEX,
  PTR_INDEX_NUSW,
  PTR_INDEX_NUW,
  OFFSET,
  PTR_DIFF,
  ADDR,
  PTR_TO_INT,
  PTR_FROM_INT,
  PTR_TO_BITS,
  PTR_FROM_BITS,
  PTR_EQ,
  IS_OVERLAP
};

namespace typeCheck {
namespace ptrType {
/// ptr(w) x (bv(w) x MPZ)^m x MPZ x MPZ -> ptr(w) (S2e): the base a
/// pointer, each index a w-bit integer; arity 2m + 3 (m >= 0: a
/// constant-only node is legal, PtrIndex.md rev 6), the stride, constant
/// and K arguments MPZ terminals.
struct PtrIndex : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    size_t a = exp->arity();
    if (a < 3 || a % 2 == 0)
      return sort::errorTy(exp->efac());
    for (size_t k = 2; k + 2 < a; k += 2)
      if (!isOpX<MPZ>(exp->arg(k)))
        return sort::errorTy(exp->efac());
    if (!isOpX<MPZ>(exp->arg(a - 2)) || !isOpX<MPZ>(exp->arg(a - 1)))
      return sort::errorTy(exp->efac());
    Expr b = tc.typeOf(exp->first());
    if (!isOpX<PTRSORT>(b))
      return sort::errorTy(exp->efac());
    Expr iw = bv::bvsort(ptr::width(b), exp->efac());
    for (size_t k = 1; k + 2 < a; k += 2)
      if (tc.typeOf(exp->arg(k)) != iw)
        return sort::errorTy(exp->efac());
    return b;
  }
};
/// OFFSET(x_1, s_1, ..., x_m, s_m, c, K): the sort of x_1; arity 2m + 2,
/// m >= 1, the stride, constant and K arguments MPZ terminals.
struct Offset : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    size_t a = exp->arity();
    if (a < 4 || a % 2 == 1)
      return sort::errorTy(exp->efac());
    for (size_t k = 1; k < a; k += 2)
      if (!isOpX<MPZ>(exp->arg(k)))
        return sort::errorTy(exp->efac());
    if (!isOpX<MPZ>(exp->arg(a - 2)))
      return sort::errorTy(exp->efac());
    return tc.typeOf(exp->first());
  }
};
/// PTR_DIFF(p, q): ptr(w) x ptr(w) -> bv(w) (S2e).
struct PtrDiff : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    if (exp->arity() != 2)
      return sort::errorTy(exp->efac());
    Expr t = tc.typeOf(exp->arg(0));
    if (!isOpX<PTRSORT>(t) || t != tc.typeOf(exp->arg(1)))
      return sort::errorTy(exp->efac());
    return bv::bvsort(ptr::width(t), exp->efac());
  }
};
/// An observation marker (ADDR, PTR_TO_INT, PTR_TO_BITS): ptr(w) -> bv(w).
struct Observation : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    if (exp->arity() != 1)
      return sort::errorTy(exp->efac());
    Expr t = tc.typeOf(exp->first());
    if (!isOpX<PTRSORT>(t))
      return sort::errorTy(exp->efac());
    return bv::bvsort(ptr::width(t), exp->efac());
  }
};
/// A constructor marker (PTR_FROM_INT, PTR_FROM_BITS): bv(w) -> ptr(w).
struct Constructor : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    if (exp->arity() != 1)
      return sort::errorTy(exp->efac());
    Expr t = tc.typeOf(exp->first());
    if (!isOpX<BVSORT>(t))
      return sort::errorTy(exp->efac());
    return ptr::ptrsort(bv::width(t), exp->efac());
  }
};
/// PTR_EQ(p, q): ptr(w) x ptr(w) -> Bool (S2e).
struct PtrEq : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    if (exp->arity() != 2)
      return sort::errorTy(exp->efac());
    Expr t = tc.typeOf(exp->arg(0));
    if (!isOpX<PTRSORT>(t) || t != tc.typeOf(exp->arg(1)))
      return sort::errorTy(exp->efac());
    return sort::boolTy(exp->efac());
  }
};
/// IS_OVERLAP(p, n, q, m): ptr(w) x MPZ x ptr(w) x MPZ -> Bool, n, m >= 1.
struct IsOverlap : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    if (exp->arity() != 4 || !isOpX<MPZ>(exp->arg(1)) ||
        !isOpX<MPZ>(exp->arg(3)))
      return sort::errorTy(exp->efac());
    Expr t = tc.typeOf(exp->arg(0));
    if (!isOpX<PTRSORT>(t) || t != tc.typeOf(exp->arg(2)))
      return sort::errorTy(exp->efac());
    return sort::boolTy(exp->efac());
  }
};
} // namespace ptrType
} // namespace typeCheck

NOP_BASE(PtrOp)
/// The offset part of a PTR_INDEX, without base and promise (PtrSort.md, B1;
/// S1.4): value sum x_k * s_k + c + K (mod 2^n). view() gives it as the
/// offset of a node chain over an object; the rules that build integer terms
/// from an offset expand it at once (SeaM `offset.flat`), so it lives only
/// inside a PtrView. No solver knows it (the boundary guard checks).
NOP(OFFSET, "offset", FUNCTIONAL, PtrOp, typeCheck::ptrType::Offset)
/// The difference of two pointers, addr(p) - addr(q) (`sea.addr.diff`;
/// PtrSort.md 1.6, S1.5). SeaM's `diff.*` rules; erased to bvsub at the
/// boundary (PtrIndexExpander).
NOP(PTR_DIFF, "ptr-diff", FUNCTIONAL, PtrOp, typeCheck::ptrType::PtrDiff)
/// The markers (PtrSort.md 1.4, S1.6): identities that name why a pointer
/// leaves or enters the sort. Observations: ADDR (an access or an ordered
/// compare, the ALIGN premise, a promise), PTR_TO_INT (an escape,
/// sea.addr.to_int), PTR_TO_BITS (a stored pointer). Constructors:
/// PTR_FROM_INT (provenance lost, sea.addr.from_int), PTR_FROM_BITS (a loaded
/// pointer). Never rewritten into one another; erased to their argument at
/// the boundary (PtrIndexExpander).
NOP(ADDR, "addr", FUNCTIONAL, PtrOp, typeCheck::ptrType::Observation)
NOP(PTR_TO_INT, "ptr-to-int", FUNCTIONAL, PtrOp,
    typeCheck::ptrType::Observation)
NOP(PTR_FROM_INT, "ptr-from-int", FUNCTIONAL, PtrOp,
    typeCheck::ptrType::Constructor)
NOP(PTR_TO_BITS, "ptr-to-bits", FUNCTIONAL, PtrOp,
    typeCheck::ptrType::Observation)
NOP(PTR_FROM_BITS, "ptr-from-bits", FUNCTIONAL, PtrOp,
    typeCheck::ptrType::Constructor)
/// Pointer equality (PtrSort.md D6, S2d): addr(p) = addr(q), the compare of
/// sea.addr.cmp eq / ne and of two access addresses in a ROW step. A
/// pointer compare is told apart from a cell or Boolean compare by its
/// operator (the bit-vector rules never see it); erased to EQ at the
/// boundary (PtrIndexExpander).
NOP(PTR_EQ, "ptr-eq", FUNCTIONAL, PtrOp, typeCheck::ptrType::PtrEq)
/// An alias question the rewriter left open (PtrSort D13, S4): unresolved
/// alias queries are explicit in the formula, to count
/// them and later to pick an aliasing scenario as a named assumption. Do
/// the n units at p and the m units at q share an address? On the circle
/// of addresses: addr(q) - addr(p) <u n or addr(p) - addr(q) <u m; for
/// n = m = 1 that is addr(p) = addr(q), erased to the EQ a PTR_EQ erases
/// to. Built by the ROW steps (rowCompare, W-ROW-*) for the compare they
/// leave; program compares stay PTR_EQ.
NOP(IS_OVERLAP, "overlap", FUNCTIONAL, PtrOp, typeCheck::ptrType::IsOverlap)
NOP(PTR_INDEX, "ptr-index", FUNCTIONAL, PtrOp, typeCheck::ptrType::PtrIndex)
NOP(PTR_INDEX_NUSW, "ptr-index-nusw", FUNCTIONAL, PtrOp,
    typeCheck::ptrType::PtrIndex)
NOP(PTR_INDEX_NUW, "ptr-index-nuw", FUNCTIONAL, PtrOp,
    typeCheck::ptrType::PtrIndex)

namespace ptr {
inline bool isPtrIndex(ENode *e) {
  return isOpX<PTR_INDEX>(e) || isOpX<PTR_INDEX_NUSW>(e) ||
         isOpX<PTR_INDEX_NUW>(e);
}
inline bool isPtrIndex(const Expr &e) { return isPtrIndex(e.get()); }
/// One of the five markers (identities at the boundary).
inline bool isMarker(ENode *e) {
  return isOpX<ADDR>(e) || isOpX<PTR_TO_INT>(e) || isOpX<PTR_FROM_INT>(e) ||
         isOpX<PTR_TO_BITS>(e) || isOpX<PTR_FROM_BITS>(e);
}
inline bool isMarker(const Expr &e) { return isMarker(e.get()); }
/// An observation marker (ADDR, PTR_TO_INT, PTR_TO_BITS): its value is the
/// address of its argument, so a reader may see through it (sound under the
/// sort too; PTR_FROM_INT / PTR_FROM_BITS are constructors: no see-through,
/// D17).
inline bool isObservation(const Expr &e) {
  return isOpX<ADDR>(e) || isOpX<PTR_TO_INT>(e) || isOpX<PTR_TO_BITS>(e);
}
/// \p e without its observation markers.
inline Expr stripObservation(Expr e) {
  while (isObservation(e))
    e = e->arg(0);
  return e;
}

/// The address width of \p a (a pointer or an address): a node's base's,
/// else its sort's (bv(w) or ptr(w)). Structural through nodes, so a node
/// whose base is a bit-vector (a unit driver's input) has one too.
inline unsigned addrWidth(const Expr &a, TypeChecker &tc) {
  Expr p = a;
  while (isPtrIndex(p))
    p = p->arg(0); // the base
  return addrSortWidth(tc.typeOf(p));
}

/// IS_OVERLAP(p, n, q, m) (n, m >= 1 units).
inline Expr overlap(const Expr &p, unsigned long n, const Expr &q,
                    unsigned long m) {
  ExprFactory &ef = p->efac();
  ExprVector args{p, mkTerm(mpz_class(n), ef), q, mkTerm(mpz_class(m), ef)};
  return mknary<IS_OVERLAP>(args.begin(), args.end());
}
inline bool isUnitOverlap(const Expr &e) {
  return isOpX<IS_OVERLAP>(e) && getTerm<mpz_class>(e->arg(1)) == 1 &&
         getTerm<mpz_class>(e->arg(3)) == 1;
}

/// OFFSET over the (index, stride) pairs (m >= 1), c and K.
inline Expr offset(const std::vector<std::pair<Expr, mpz_class>> &idx,
                   const mpz_class &c, const mpz_class &K) {
  ExprFactory &ef = idx.front().first->efac();
  ExprVector args;
  for (const auto &[x, s] : idx) {
    args.push_back(x);
    args.push_back(mkTerm<mpz_class>(s, ef));
  }
  args.push_back(mkTerm<mpz_class>(c, ef));
  args.push_back(mkTerm<mpz_class>(K, ef));
  return mknary<OFFSET>(args.begin(), args.end());
}
inline size_t offsetIndices(const Expr &e) { return (e->arity() - 2) / 2; }
inline Expr offsetIndex(const Expr &e, size_t k) { return e->arg(2 * k); }
inline mpz_class offsetStride(const Expr &e, size_t k) {
  return getTerm<mpz_class>(e->arg(2 * k + 1));
}
/// A PTR_INDEX node with a promise.
inline bool isFlagged(ENode *e) {
  return isOpX<PTR_INDEX_NUSW>(e) || isOpX<PTR_INDEX_NUW>(e);
}

inline size_t numIndices(const Expr &e) { return (e->arity() - 3) / 2; }
inline Expr base(const Expr &e) { return e->arg(0); }
inline Expr index(const Expr &e, size_t k) { return e->arg(1 + 2 * k); }
inline mpz_class stride(const Expr &e, size_t k) {
  return getTerm<mpz_class>(e->arg(2 + 2 * k));
}
inline mpz_class constant(const Expr &e) {
  return getTerm<mpz_class>(e->arg(e->arity() - 2));
}
inline mpz_class byteOffset(const Expr &e) {
  return getTerm<mpz_class>(e->arg(e->arity() - 1));
}

/// PTR_INDEX of \p kind over b with the (index, stride) pairs, c and K.
inline Expr ptrIndex(PtrOpKind kind, Expr b,
                     const std::vector<std::pair<Expr, mpz_class>> &idx,
                     const mpz_class &c, const mpz_class &K) {
  ExprFactory &ef = b->efac();
  ExprVector args{b};
  for (const auto &[x, s] : idx) {
    args.push_back(x);
    args.push_back(mkTerm<mpz_class>(s, ef));
  }
  args.push_back(mkTerm<mpz_class>(c, ef));
  args.push_back(mkTerm<mpz_class>(K, ef));
  switch (kind) {
  case PtrOpKind::PTR_INDEX_NUSW:
    return mknary<PTR_INDEX_NUSW>(args.begin(), args.end());
  case PtrOpKind::PTR_INDEX_NUW:
    return mknary<PTR_INDEX_NUW>(args.begin(), args.end());
  default:
    return mknary<PTR_INDEX>(args.begin(), args.end());
  }
}

/// The address of byte \p k of an access at \p a, as a K-node (PtrIndex.md
/// section 4, K writers): a flagged node gets K + k (same operator); an ite
/// whose every leaf is a flagged node gets k pushed into its leaves (exact).
/// Null otherwise (the caller adds k with bvadd). Written for the bytes of
/// an access at \p a (0 <= k < its width), where the stride rule's lemma
/// holds under that access's A-INB; also for a program `ptr.add(node, k)`
/// (k signed), where it need not: in the Ackermann case the rule's firing
/// never affects the verdict (PtrIndex.md 0b), only the rerun rate; R-STRIDE
/// accepts a K != 0 only in the range where A-INB excludes a wrap
/// (PtrIndex.md 0c).
inline Expr withByteOffset(const Expr &a, const mpz_class &k) {
  if (isFlagged(a.get())) {
    ExprVector kids(a->args_begin(), a->args_end());
    kids.back() = mkTerm<mpz_class>(byteOffset(a) + k, a->efac());
    return a->efac().mkNary(a->op(), kids);
  }
  if (isOpX<ITE>(a)) {
    Expr t = withByteOffset(a->arg(1), k);
    if (!t)
      return Expr();
    Expr e = withByteOffset(a->arg(2), k);
    if (!e)
      return Expr();
    return mk<ITE>(a->arg(0), t, e);
  }
  return Expr();
}

/// The address a + o as a node (PtrSort rev 5, S1: every pointer addition
/// is a PTR_INDEX node; step (a)'s flat form reads it, so a query is the
/// bvadd's):
///  - o a numeral: a promised node (or an ite of them) gets K + o (o
///    signed; the promise kept, withByteOffset); an unflagged node merges it
///    into c (mod 2^n); otherwise PTR_INDEX(a, c = o);
///  - o symbolic: an unflagged node gets the index (o, 1) appended;
///    otherwise PTR_INDEX(a, [o] * 1) (a promised node stays the base: its
///    promise is about its own address).
inline Expr addressPlus(const Expr &a, const Expr &o) {
  const bool unflagged = isPtrIndex(a) && !isFlagged(a.get());
  auto indicesOf = [](const Expr &nd) {
    std::vector<std::pair<Expr, mpz_class>> idx;
    for (size_t i = 0; i < numIndices(nd); ++i)
      idx.push_back({index(nd, i), stride(nd, i)});
    return idx;
  };
  unsigned n;
  if (bv::isBvNum(o, n)) {
    mpz_class k = bv::toMpz(o), m;
    mpz_ui_pow_ui(m.get_mpz_t(), 2, n);
    mpz_fdiv_r(k.get_mpz_t(), k.get_mpz_t(), m.get_mpz_t());
    mpz_class half;
    mpz_ui_pow_ui(half.get_mpz_t(), 2, n - 1);
    mpz_class ks = k >= half ? mpz_class(k - m) : k;
    if (Expr r = withByteOffset(a, ks))
      return r;
    if (unflagged) {
      mpz_class c = constant(a) + k, r;
      mpz_fdiv_r(r.get_mpz_t(), c.get_mpz_t(), m.get_mpz_t());
      return ptrIndex(PtrOpKind::PTR_INDEX, base(a), indicesOf(a), r,
                      byteOffset(a));
    }
    return ptrIndex(PtrOpKind::PTR_INDEX, a, {}, k, mpz_class(0L));
  }
  if (unflagged) {
    auto idx = indicesOf(a);
    idx.push_back({o, mpz_class(1L)});
    return ptrIndex(PtrOpKind::PTR_INDEX, base(a), idx, constant(a),
                    byteOffset(a));
  }
  return ptrIndex(PtrOpKind::PTR_INDEX, a, {{o, mpz_class(1L)}}, mpz_class(0L),
                  mpz_class(0L));
}

/// Result of byteAddress: how the offset was placed.
enum class OffsetPush { None, Pushed, Refused };

/// The address of byte \p k (a numeral) of an access at \p a, with k pushed
/// into the leaves of an ite address (exact, mod 2^n):
///   ite(c, x, y) + k  ~>  ite(c, x + k, y + k)
/// each leaf by addressPlus (a node merges k into c or K, another leaf gets
/// PTR_INDEX(leaf, c = k)). ite(c, A + 24, A) + 3 is ite(c, A + 27, A + 3):
/// a leaf compare A + 27 = A + j folds by rewriting, where the bvadd over
/// the ite left it to the solver's adders (z3 23.6 s -> 0.17 s on a W-ITE
/// check obligation). Only when the ite spine of \p a has at most
/// \p maxLeaves distinct leaves and at most as many distinct ite nodes
/// (shared subterms counted once and rebuilt once: the growth per byte is
/// the spine's size); otherwise, or with maxLeaves = 0, addressPlus(a, k)
/// (Refused for an ite over the cap). \p how: Pushed when this pushed k
/// where addressPlus would not have (an ite of promised nodes is
/// addressPlus's own push, None).
inline Expr byteAddress(const Expr &a, const Expr &k, unsigned maxLeaves,
                        OffsetPush &how) {
  how = OffsetPush::None;
  Expr plain = addressPlus(a, k);
  if (!isOpX<ITE>(a) || isOpX<ITE>(plain) || maxLeaves == 0)
    return plain;
  // the spine: distinct ite nodes and leaves, within the cap
  std::unordered_set<ENode *> ites, leaves;
  std::vector<ENode *> stack{a.get()};
  while (!stack.empty()) {
    ENode *n = stack.back();
    stack.pop_back();
    if (!isOpX<ITE>(n)) {
      leaves.insert(n);
    } else if (ites.insert(n).second) {
      stack.push_back(n->arg(1));
      stack.push_back(n->arg(2));
    }
    if (leaves.size() > maxLeaves || ites.size() > maxLeaves) {
      how = OffsetPush::Refused;
      return plain;
    }
  }
  std::unordered_map<ENode *, Expr> memo;
  std::function<Expr(const Expr &)> push = [&](const Expr &e) -> Expr {
    auto it = memo.find(e.get());
    if (it != memo.end())
      return it->second;
    Expr r = isOpX<ITE>(e)
                 ? mk<ITE>(e->arg(0), push(e->arg(1)), push(e->arg(2)))
                 : addressPlus(e, k);
    memo.emplace(e.get(), r);
    return r;
  };
  how = OffsetPush::Pushed;
  return push(a);
}

/// The promise of a flagged node N = PTR_INDEX_F(b, x_k, s_k, c, K) as a
/// Boolean bit-vector formula over its arguments (PtrIndex.md section 1,
/// order-free; K is not part of it): in w = 2n + 8 + ceil(log2(m + 2)) bits,
/// exact integers,
///   NUSW: x_k read signed (sext), s_k and c as stored (signed);
///   NUW:  x_k read unsigned (zext), s_k and c as stored (unsigned);
///   the stored s_k and c are n-bit values, signed in [-2^(n-1), 2^(n-1))
///   for NUSW and unsigned in [0, 2^n) for NUW (PtrIndex.md section 2: a
///   NUW attribute -1 is stored as 2^n - 1, the interpreter's reading);
///   a stored value outside that range would make this formula disagree
///   with Semantics.cpp;
///   P1: each x_k * s_k fits n bits (signed: [-2^(n-1), 2^(n-1)); unsigned:
///       [0, 2^n));
///   P2: O = sum x_k * s_k + c fits n bits the same way;
///   P3: b + O (b unsigned) is in [0, 2^n).
/// The checks of Semantics.cpp brokenPtrIndexPromise, as a formula: the
/// premise of a firing that uses the promise (--seam-check), and the
/// paranoid pass's assertion later. \p n the width of b.
inline Expr promise(const Expr &node, unsigned n) {
  ExprFactory &ef = node->efac();
  const bool sgn = isOpX<PTR_INDEX_NUSW>(node);
  const size_t m = numIndices(node);
  unsigned lg = 0;
  while ((size_t(1) << lg) < m + 2)
    ++lg;
  const unsigned w = 2 * n + 8 + lg;
  auto num = [&](const mpz_class &v) {
    mpz_class mod, r;
    mpz_ui_pow_ui(mod.get_mpz_t(), 2, w);
    mpz_fdiv_r(r.get_mpz_t(), v.get_mpz_t(), mod.get_mpz_t());
    return bv::bvnum(r, w, ef);
  };
  mpz_class lo = 0L, hi, two;
  mpz_ui_pow_ui(two.get_mpz_t(), 2, n);
  if (sgn) {
    mpz_class h;
    mpz_ui_pow_ui(h.get_mpz_t(), 2, n - 1);
    lo = mpz_class(0L) - h;
    hi = h - mpz_class(1L);
  } else {
    hi = two - mpz_class(1L);
  }
  // the integer range, as signed w-bit compares (w > n + 1: exact)
  auto fits = [&](const Expr &t, const mpz_class &a, const mpz_class &z) {
    return mk<AND>(mk<BSLE>(num(a), t), mk<BSLE>(t, num(z)));
  };
  auto ext = [&](const Expr &x) {
    return sgn ? bv::sext(x, w) : bv::zext(x, w);
  };
  ExprVector conj;
  Expr total = num(constant(node));
  for (size_t k = 0; k < m; ++k) {
    Expr prod = mk<BMUL>(ext(index(node, k)), num(stride(node, k)));
    conj.push_back(fits(prod, lo, hi)); // P1
    total = mk<BADD>(total, prod);
  }
  conj.push_back(fits(total, lo, hi)); // P2
  Expr addr = mk<BADD>(bv::zext(mk<ADDR>(base(node)), w), total);
  conj.push_back(fits(addr, mpz_class(0L), two - mpz_class(1L))); // P3
  return mknary<AND>(conj.begin(), conj.end());
}

/// The same node with its arguments \p kids under the unflagged operator
/// (view()'s rebasing strips the promise; see PtrIndex.md).
inline Expr unflagged(const ExprVector &kids) {
  return mknary<PTR_INDEX>(kids.begin(), kids.end());
}
} // namespace ptr
} // namespace op

/// Expands every PTR_INDEX node into b + sum x_k*s_k + c + K (bit-vector
/// arithmetic, n bits); the rest of the term unchanged. One instance keeps
/// its memo (and the pinned keys) for its lifetime.
///
/// The default arithmetic is the term the lowering (sea-lower-ptr-index,
/// then the VC's encoding of mul / add / addr.add) gives:
///   bvadd(b, off), off = ((x_1*s_1 + x_2*s_2) + ...) + (c + K)
/// with x_k for x_k*1 (an addr.add's offset; a ptr.index stride 1 too)
/// with no numeral addend when c + K = 0 and off = c + K with no index.
/// setArith replaces it (SeaM: the normalized sum, SeaMRules.hh flat()); the
/// arithmetic of a node is applied after its arguments are expanded.
class PtrIndexExpander {
public:
  using Arith = std::function<Expr(const Expr &)>;
  void setArith(Arith f) { m_arith = std::move(f); }

  Expr expand(const Expr &root) {
    if (!has(root))
      return root;
    std::vector<std::pair<ENode *, bool>> stack{{root.get(), false}};
    while (!stack.empty()) {
      auto [n, done] = stack.back();
      if (m_memo.count(n)) {
        stack.pop_back();
        continue;
      }
      if (!done) {
        stack.back().second = true;
        for (size_t i = 0; i < n->arity(); ++i)
          if (!m_memo.count(n->arg(i)))
            stack.push_back({n->arg(i), false});
        continue;
      }
      stack.pop_back();
      ExprVector kids;
      bool changed = false;
      for (size_t i = 0; i < n->arity(); ++i) {
        Expr k = m_memo.at(n->arg(i));
        changed |= k.get() != n->arg(i);
        kids.push_back(k);
      }
      Expr res = changed ? n->efac().mkNary(n->op(), kids) : Expr(n);
      if (op::ptr::isPtrIndex(res.get())) {
        ++m_expanded;
        res = m_arith ? m_arith(res) : arith(res);
      } else if (isOpX<PTR_DIFF>(res)) {
        res = mk<BSUB>(res->arg(0), res->arg(1));
      } else if (op::ptr::isMarker(res)) {
        res = res->arg(0);
      } else if (isOpX<PTR_EQ>(res)) {
        res = mk<EQ>(res->arg(0), res->arg(1));
      } else if (isOpX<IS_OVERLAP>(res)) {
        res = overlapArith(res);
      } else if (isOpX<OFFSET>(res)) {
        res = offsetArith(res);
      } else if (isOpX<PTRSORT>(res)) {
        // the sort renaming (S2e): a constant, an fdecl, an array sort
        // whose kid is ptr(w) is rebuilt over bv(w) with the same name
        res = bv::bvsort(ptr::width(res), res->efac());
      }
      m_keep.push_back(Expr(n));
      m_keep.push_back(res);
      m_memo.emplace(n, res);
    }
    return m_memo.at(root.get());
  }

  /// Does \p e contain a PTR_INDEX node? (memoized, keys pinned)
  bool has(const Expr &e) {
    auto it = m_has.find(e.get());
    if (it != m_has.end())
      return it->second;
    std::vector<std::pair<ENode *, bool>> stack{{e.get(), false}};
    while (!stack.empty()) {
      auto [n, done] = stack.back();
      stack.pop_back();
      if (m_has.count(n))
        continue;
      if (op::ptr::isPtrIndex(n) || isOpX<OFFSET>(n) || isOpX<PTR_DIFF>(n) ||
          op::ptr::isMarker(n) || isOpX<PTR_EQ>(n) || isOpX<IS_OVERLAP>(n) ||
          isOpX<PTRSORT>(n)) {
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
      m_pinned.push_back(Expr(n));
    }
    return m_has.at(e.get());
  }

  uint64_t expanded() const { return m_expanded; }

private:
  /// IS_OVERLAP over erased arguments: EQ for unit widths, else the circular
  /// interval test
  Expr overlapArith(const Expr &e) {
    using namespace op;
    const Expr p = e->arg(0), q = e->arg(2);
    const mpz_class n = getTerm<mpz_class>(e->arg(1)),
                    m = getTerm<mpz_class>(e->arg(3));
    if (n == 1 && m == 1)
      return mk<EQ>(p, q);
    unsigned w = bv::width(m_tc.typeOf(p));
    ExprFactory &ef = e->efac();
    return mk<OR>(mk<BULT>(mk<BSUB>(q, p), bv::bvnum(n, w, ef)),
                  mk<BULT>(mk<BSUB>(p, q), bv::bvnum(m, w, ef)));
  }

  /// OFFSET(x_1, s_1, ..., c, K) as sum x_k * s_k + c + K (an OFFSET lives
  /// in a PtrView; this also expands an OFFSET encountered at the boundary)
  Expr offsetArith(const Expr &e) {
    using namespace op;
    unsigned n = bv::width(m_tc.typeOf(ptr::offsetIndex(e, 0)));
    ExprFactory &ef = e->efac();
    auto num = [&](const mpz_class &v) {
      mpz_class mod, r;
      mpz_ui_pow_ui(mod.get_mpz_t(), 2, n);
      mpz_fdiv_r(r.get_mpz_t(), v.get_mpz_t(), mod.get_mpz_t());
      return bv::bvnum(r, n, ef);
    };
    Expr off;
    for (size_t k = 0; k < ptr::offsetIndices(e); ++k) {
      Expr t = mk<BMUL>(ptr::offsetIndex(e, k), num(ptr::offsetStride(e, k)));
      off = off ? Expr(mk<BADD>(off, t)) : t;
    }
    mpz_class ck = getTerm<mpz_class>(e->arg(e->arity() - 2)) +
                   getTerm<mpz_class>(e->arg(e->arity() - 1));
    return ck == 0 ? off : Expr(mk<BADD>(off, num(ck)));
  }

  Expr arith(const Expr &e) {
    using namespace op;
    Expr b = ptr::base(e);
    unsigned n = ptr::addrSortWidth(m_tc.typeOf(b));
    ExprFactory &ef = e->efac();
    auto num = [&](const mpz_class &v) {
      mpz_class mod, r;
      mpz_ui_pow_ui(mod.get_mpz_t(), 2, n);
      mpz_fdiv_r(r.get_mpz_t(), v.get_mpz_t(), mod.get_mpz_t());
      return bv::bvnum(r, n, ef);
    };
    Expr off;
    for (size_t k = 0; k < ptr::numIndices(e); ++k) {
      // stride 1 (an addr.add's offset, S2g): the index itself
      Expr s = num(ptr::stride(e, k));
      Expr t = bv::toMpz(s) == 1 ? ptr::index(e, k)
                                 : Expr(mk<BMUL>(ptr::index(e, k), s));
      off = off ? Expr(mk<BADD>(off, t)) : t;
    }
    mpz_class ck;
    mpz_add(ck.get_mpz_t(), ptr::constant(e).get_mpz_t(),
            ptr::byteOffset(e).get_mpz_t());
    if (!off)
      off = num(ck);
    else if (ck != mpz_class(0L))
      off = mk<BADD>(off, num(ck));
    return mk<BADD>(b, off);
  }

  TypeChecker m_tc;
  Arith m_arith;
  std::unordered_map<ENode *, Expr> m_memo;
  std::unordered_map<ENode *, bool> m_has;
  std::vector<Expr> m_keep, m_pinned;
  uint64_t m_expanded = 0;
};

} // namespace expr
