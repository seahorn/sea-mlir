// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifdef WITH_BITWUZLA

#include "sea/Expr/Smt/MarshalBitwuzla.hh"
#include "sea/Expr/ExprInterp.hh"
#include "sea/Expr/ExprOpBinder.hh"

#include "llvm/Support/raw_ostream.h"

#include "boost/lexical_cast.hpp"

#include <optional>

using namespace expr;
using bitwuzla::Kind;
using bitwuzla::Sort;
using bitwuzla::Term;
using bitwuzla::TermManager;

namespace sea {
namespace solver {

[[noreturn]] static void unsupported(Expr e, const char *what) {
  std::string str;
  llvm::raw_string_ostream os(str);
  os << what << ": " << *e;
  throw bitwuzla_unsupported(os.str());
}

[[noreturn]] static void unsupported(const std::string &what) {
  throw bitwuzla_unsupported(what);
}

std::string marshal_bitwuzla::name_of(Expr e) {
  Expr fname;
  if (bind::isFapp(e)) {
    // name of the app
    fname = bind::fname(e);
    // name of the fdecl
    fname = bind::fname(fname);
  } else if (bind::isFdecl(e)) {
    // name of the fdecl
    fname = bind::fname(e);
  } else {
    fname = e;
  }

  std::string sname;
  if (isOpX<STRING>(fname))
    sname = getTerm<std::string>(fname);
  else
    sname = boost::lexical_cast<std::string>(*fname);
  return sname;
}

Sort marshal_bitwuzla::encode_type(Expr e, TermManager &tm) {
  if (isOpX<BOOL_TY>(e))
    return tm.mk_bool_sort();
  if (isOpX<BVSORT>(e))
    return tm.mk_bv_sort(bv::width(e));
  if (isOpX<ARRAY_TY>(e))
    return tm.mk_array_sort(marshal_bitwuzla::encode_type(e->left(), tm),
                            encode_type(e->right(), tm));
  if (isOpX<INT_TY>(e) || isOpX<REAL_TY>(e))
    unsupported(e, "integer and real sorts are not supported in Bitwuzla");
  unsupported(e, "unhandled sort");
}

Expr marshal_bitwuzla::decode_type(const Sort &sort, ExprFactory &efac) {
  if (sort.is_bool())
    return sort::boolTy(efac);
  if (sort.is_bv())
    return bv::bvsort(sort.bv_size(), efac);
  if (sort.is_array())
    return sort::arrayTy(decode_type(sort.array_index(), efac),
                         decode_type(sort.array_element(), efac));
  unsupported("unhandled Bitwuzla sort: " + sort.str());
}

namespace {
using Args = std::vector<Term>;

/// Left fold of an associative operator (SMT-LIB's :left-assoc).
Term foldLeft(TermManager &tm, Kind k, const Args &args) {
  Term res = args[0];
  for (size_t i = 1; i < args.size(); ++i)
    res = tm.mk_term(k, {res, args[i]});
  return res;
}

/// bvumulo / bvsmulo split like z3's no-overflow / no-underflow predicates
/// (which Expr's mulOverflows expects): a signed product overflows upward
/// when the signs agree, downward when they differ.
Term signedMulNo(TermManager &tm, const Args &a, bool upward) {
  Sort s = a[0].sort();
  uint64_t w = s.bv_size();
  Term ovf = tm.mk_term(Kind::BV_SMUL_OVERFLOW, {a[0], a[1]});
  Term sa = tm.mk_term(Kind::BV_EXTRACT, {a[0]}, {w - 1, w - 1});
  Term sb = tm.mk_term(Kind::BV_EXTRACT, {a[1]}, {w - 1, w - 1});
  Term same = tm.mk_term(Kind::EQUAL, {sa, sb});
  Term dir = upward ? same : tm.mk_term(Kind::NOT, {same});
  return tm.mk_term(Kind::NOT, {tm.mk_term(Kind::AND, {ovf, dir})});
}
} // namespace

namespace {
/// The sub-terms encode_node needs encoded first: the others are sorts,
/// indices and other non-terms. Mirrors the cases of encode_node.
void kidsOf(Expr e, std::vector<Expr> &kids) {
  if (bind::isBVar(e) || isOpX<UINT>(e) || isOpX<MPQ>(e) || isOpX<MPZ>(e) ||
      bv::is_bvnum(e) ||
      (isOpX<FAPP>(e) && e->arity() == 1 && bind::isFdecl(e->left())) ||
      bind::isFdecl(e) || isOpX<FORALL>(e) || isOpX<EXISTS>(e) ||
      isOpX<LAMBDA>(e))
    return;
  if (isOpX<CONST_ARRAY>(e))
    kids.push_back(e->right());
  else if (isOpX<BEXTRACT>(e))
    kids.push_back(bv::earg(e));
  else if (isOpX<BSEXT>(e) || isOpX<BZEXT>(e))
    kids.push_back(e->left());
  else
    kids.insert(kids.end(), e->args_begin(), e->args_end());
}

/// the encoding of \p e, whose kids (see kidsOf) are in the cache
Term encode_node(Expr e, TermManager &tm, bzcache_t &cache);

Term encoded(Expr e, TermManager &tm, bzcache_t &cache) {
  if (isOpX<TRUE>(e))
    return tm.mk_true();
  if (isOpX<FALSE>(e))
    return tm.mk_false();
  auto it = cache.find(e);
  assert(it != cache.end());
  return it->second;
}
} // namespace

Term marshal_bitwuzla::encode_term(Expr e, TermManager &tm, bzcache_t &cache) {
  assert(e);
  // -- explicit stack: a chain of stores can be thousands deep
  std::vector<Expr> todo{e}, kids;
  while (!todo.empty()) {
    Expr x = todo.back();
    if (isOpX<TRUE>(x) || isOpX<FALSE>(x) || cache.count(x)) {
      todo.pop_back();
      continue;
    }
    kids.clear();
    kidsOf(x, kids);
    size_t n = todo.size();
    for (Expr k : kids)
      if (!isOpX<TRUE>(k) && !isOpX<FALSE>(k) && !cache.count(k))
        todo.push_back(k);
    if (todo.size() > n)
      continue;
    todo.pop_back();
    // -- cache the result for unmarshaling
    cache[x] = encode_node(x, tm, cache);
  }
  return encoded(e, tm, cache);
}

namespace {
Term encode_node(Expr e, TermManager &tm, bzcache_t &cache) {
  Term res;
  if (bind::isBVar(e)) {
    unsupported(e, "bound variable outside of a binder");
  } else if (isOpX<UINT>(e) || isOpX<MPQ>(e) || isOpX<MPZ>(e)) {
    unsupported(e, "integer and real numerals are not supported in Bitwuzla");
  } else if (bv::is_bvnum(e)) {
    unsigned w = bv::widthBvNum(e);
    // -- a numeral outside [0, 2^w) wraps (yices_bvconst_mpz does the same)
    expr::mpz_class n;
    mpz_fdiv_r_2exp(n.get_mpz_t(), bv::toMpz(e).get_mpz_t(), w);
    res = tm.mk_bv_value(tm.mk_bv_sort(w), n.to_string(10), 10);
  } else if (isOpX<FAPP>(e) && e->arity() == 1 && bind::isFdecl(e->left())) {
    // -- an uninterpreted constant (any sort Bitwuzla has)
    res =
        tm.mk_const(marshal_bitwuzla::encode_type(bind::rangeTy(e->left()), tm),
                    marshal_bitwuzla::name_of(e));
  } else if (bind::isFdecl(e)) {
    /** function declaration */
    std::vector<Sort> domain;
    for (size_t i = 0; i < bind::domainSz(e); ++i)
      domain.push_back(marshal_bitwuzla::encode_type(bind::domainTy(e, i), tm));
    Sort range = marshal_bitwuzla::encode_type(bind::rangeTy(e), tm);
    res = tm.mk_const(tm.mk_fun_sort(domain, range),
                      marshal_bitwuzla::name_of(e));
  } else if (bind::isFapp(e)) {
    /** function application */
    if (!bind::isFdecl(bind::fname(e)))
      unsupported(e, "application of a non-declared function");
    Args args;
    for (auto it = e->args_begin(), end = e->args_end(); it != end; ++it)
      args.push_back(encoded(*it, tm, cache));
    res = tm.mk_term(Kind::APPLY, args);
  } else if (isOpX<FORALL>(e) || isOpX<EXISTS>(e) || isOpX<LAMBDA>(e)) {
    unsupported(e, "quantifiers and lambdas are not supported by this backend");
  } else if (isOpX<CONST_ARRAY>(e)) {
    // (const-array index-sort v)
    Term v = encoded(e->right(), tm, cache);
    res = tm.mk_const_array(
        tm.mk_array_sort(marshal_bitwuzla::encode_type(e->left(), tm),
                         v.sort()),
        v);
  } else if (isOpX<BEXTRACT>(e)) {
    assert(bv::high(e) >= bv::low(e));
    res = tm.mk_term(Kind::BV_EXTRACT, {encoded(bv::earg(e), tm, cache)},
                     {bv::high(e), bv::low(e)});
  } else if (isOpX<BSEXT>(e) || isOpX<BZEXT>(e)) {
    // the second argument is the target sort
    Term t = encoded(e->left(), tm, cache);
    uint64_t from = t.sort().bv_size(), to = bv::width(e->right());
    if (to < from)
      unsupported(e, "extension to a narrower sort");
    res = to == from ? t
                     : tm.mk_term(isOpX<BSEXT>(e) ? Kind::BV_SIGN_EXTEND
                                                  : Kind::BV_ZERO_EXTEND,
                                  {t}, {to - from});
  } else {
    Args a;
    for (auto it = e->args_begin(), end = e->args_end(); it != end; ++it)
      a.push_back(encoded(*it, tm, cache));
    size_t arity = a.size();

    // -- unary and binary operators of a fixed kind
    struct Entry {
      Kind kind;
      bool assoc; ///< n-ary by left fold
    };
    auto simple = [&](Expr e) -> std::optional<Entry> {
      if (isOpX<AND>(e))
        return Entry{Kind::AND, true};
      if (isOpX<OR>(e))
        return Entry{Kind::OR, true};
      if (isOpX<XOR>(e))
        return Entry{Kind::XOR, true};
      if (isOpX<IMPL>(e))
        return Entry{Kind::IMPLIES, false};
      if (isOpX<IFF>(e))
        return Entry{Kind::EQUAL, false};
      if (isOpX<EQ>(e))
        return Entry{Kind::EQUAL, false};
      if (isOpX<NEQ>(e))
        return Entry{Kind::DISTINCT, false};
      if (isOpX<NEG>(e))
        return Entry{Kind::NOT, false};
      if (isOpX<SELECT>(e))
        return Entry{Kind::ARRAY_SELECT, false};
      if (isOpX<STORE>(e))
        return Entry{Kind::ARRAY_STORE, false};
      if (isOpX<ITE>(e))
        return Entry{Kind::ITE, false};
      if (isOpX<BNOT>(e))
        return Entry{Kind::BV_NOT, false};
      if (isOpX<BNEG>(e))
        return Entry{Kind::BV_NEG, false};
      if (isOpX<BREDAND>(e))
        return Entry{Kind::BV_REDAND, false};
      if (isOpX<BREDOR>(e))
        return Entry{Kind::BV_REDOR, false};
      if (isOpX<BAND>(e))
        return Entry{Kind::BV_AND, true};
      if (isOpX<BOR>(e))
        return Entry{Kind::BV_OR, true};
      if (isOpX<BXOR>(e))
        return Entry{Kind::BV_XOR, true};
      if (isOpX<BADD>(e))
        return Entry{Kind::BV_ADD, true};
      if (isOpX<BMUL>(e))
        return Entry{Kind::BV_MUL, true};
      if (isOpX<BCONCAT>(e))
        return Entry{Kind::BV_CONCAT, true};
      if (isOpX<BSUB>(e))
        return Entry{Kind::BV_SUB, false};
      if (isOpX<BUDIV>(e))
        return Entry{Kind::BV_UDIV, false};
      if (isOpX<BSDIV>(e))
        return Entry{Kind::BV_SDIV, false};
      if (isOpX<BUREM>(e))
        return Entry{Kind::BV_UREM, false};
      if (isOpX<BSREM>(e))
        return Entry{Kind::BV_SREM, false};
      if (isOpX<BSMOD>(e))
        return Entry{Kind::BV_SMOD, false};
      if (isOpX<BULE>(e))
        return Entry{Kind::BV_ULE, false};
      if (isOpX<BSLE>(e))
        return Entry{Kind::BV_SLE, false};
      if (isOpX<BUGE>(e))
        return Entry{Kind::BV_UGE, false};
      if (isOpX<BSGE>(e))
        return Entry{Kind::BV_SGE, false};
      if (isOpX<BULT>(e))
        return Entry{Kind::BV_ULT, false};
      if (isOpX<BSLT>(e))
        return Entry{Kind::BV_SLT, false};
      if (isOpX<BUGT>(e))
        return Entry{Kind::BV_UGT, false};
      if (isOpX<BSGT>(e))
        return Entry{Kind::BV_SGT, false};
      if (isOpX<BNAND>(e))
        return Entry{Kind::BV_NAND, false};
      if (isOpX<BNOR>(e))
        return Entry{Kind::BV_NOR, false};
      if (isOpX<BXNOR>(e))
        return Entry{Kind::BV_XNOR, false};
      // -- SMT-LIB shifts take operands of equal width, which Expr's type
      // -- checker enforces (Bitwuzla throws otherwise: reported by the solver)
      if (isOpX<BSHL>(e))
        return Entry{Kind::BV_SHL, false};
      if (isOpX<BLSHR>(e))
        return Entry{Kind::BV_SHR, false};
      if (isOpX<BASHR>(e))
        return Entry{Kind::BV_ASHR, false};
      return std::nullopt;
    };

    if (isOpX<ARRAY_DEFAULT>(e)) {
      unsupported(e, "array-default term not supported in Bitwuzla");
    } else if (isOpX<ASM>(e) && arity == 1) {
      res = a[0];
    } else if (isOpX<UMUL_NO_OVERFLOW>(e) && arity == 2) {
      res = tm.mk_term(Kind::NOT, {tm.mk_term(Kind::BV_UMUL_OVERFLOW, a)});
    } else if (isOpX<SMUL_NO_OVERFLOW>(e) && arity == 2) {
      res = signedMulNo(tm, a, /*upward=*/true);
    } else if (isOpX<SMUL_NO_UNDERFLOW>(e) && arity == 2) {
      res = signedMulNo(tm, a, /*upward=*/false);
    } else if (auto ent = simple(e)) {
      if (!ent->assoc || arity == 2) {
        // -- the argument count of fixed arity kinds (unary, ite, store, ...)
        // -- is checked by Bitwuzla, which throws on a wrong one
        res = tm.mk_term(ent->kind, a);
      } else if (arity > 2) {
        res = foldLeft(tm, ent->kind, a);
      } else {
        unsupported(e, "operator with fewer than two arguments");
      }
    } else if (isOpX<UN_MINUS>(e) || isOpX<REM>(e) || isOpX<PLUS>(e) ||
               isOpX<MINUS>(e) || isOpX<MULT>(e) || isOpX<DIV>(e) ||
               isOpX<IDIV>(e) || isOpX<MOD>(e) || isOpX<LEQ>(e) ||
               isOpX<GEQ>(e) || isOpX<LT>(e) || isOpX<GT>(e)) {
      unsupported(e, "arithmetic over integers and reals is not supported in "
                     "Bitwuzla");
    } else {
      unsupported(e, "unhandled term");
    }
  }

  return res;
}
} // namespace

Expr marshal_bitwuzla::decode_value(const Term &val, ExprFactory &efac) {
  Sort sort = val.sort();
  if (sort.is_bool()) {
    if (val.is_true())
      return mk<TRUE>(efac);
    if (val.is_false())
      return mk<FALSE>(efac);
    unsupported("Boolean model value is not a value: " + val.str());
  }
  if (sort.is_bv()) {
    if (!val.is_value())
      unsupported("bit-vector model value is not a value: " + val.str());
    return bv::bvnum(expr::mpz_class(val.value<std::string>(10), 10),
                     sort.bv_size(), efac);
  }
  if (sort.is_array()) {
    // -- a const-array under a chain of stores
    std::vector<Term> kids = val.children();
    switch (val.kind()) {
    case Kind::CONST_ARRAY:
      return op::array::constArray(decode_type(sort.array_index(), efac),
                                   decode_value(kids[0], efac));
    case Kind::ARRAY_STORE:
      return op::array::store(decode_value(kids[0], efac),
                              decode_value(kids[1], efac),
                              decode_value(kids[2], efac));
    default:
      unsupported("unexpected array model value: " + val.str());
    }
  }
  unsupported("unhandled sort of model value: " + val.str());
}

// This only works for simple expressions; constants (either atomic or
// uninterpreted function)
Expr marshal_bitwuzla::eval(Expr expr, ExprFactory &efac, TermManager &tm,
                            bzcache_t &cache, bool complete,
                            bitwuzla::Bitwuzla &bzla) {
  if (isOp<MK_STRUCT>(expr)) {
    ExprVector kids;
    for (auto arg : *expr)
      kids.push_back(eval(arg, efac, tm, cache, complete, bzla));
    return strct::mk(kids);
  } else if (op::bv::isBvNum(expr) || isOpX<MPZ>(expr) || isOpX<MPQ>(expr)) {
    return expr;
  }

  Term t = encode_term(expr, tm, cache);
  return decode_value(bzla.get_value(t), efac);
}

} // namespace solver
} // namespace sea

#endif
