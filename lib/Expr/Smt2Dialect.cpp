// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sea/Expr/Smt/Smt2Dialect.hh"

#include "sea/Expr/Expr.hh"
#include "sea/Expr/ExprAPInt.hh"
#include "sea/Expr/ExprOpBinder.hh"
#include "sea/Expr/ExprVisitor.hh"
#include "sea/Expr/TypeChecker.hh"
#include "sea/Support/Diag.h"

#include "llvm/Support/raw_ostream.h"

#include <array>

using namespace expr;

namespace sea {
namespace solver {

namespace {
/// The constructs, in the order they are reported
enum Kind : unsigned {
  K_UMUL_NOOVFL,
  K_SMUL_NOOVFL,
  K_SMUL_NOUDFL,
  K_REDOR,
  K_REDAND,
  K_CONST_ARRAY,
  K_LAMBDA,
  K_QUANTIFIER,
  K_ARRAY_MAP,
  K_ARRAY_DEFAULT,
  K_AS_ARRAY,
  K_COUNT
};

struct KindInfo {
  const char *name;
  const char *readers;
  bool expandable;
};

constexpr std::array<KindInfo, K_COUNT> kInfo = {{
    {"bvumul_noovfl", "z3 only", true},
    {"bvsmul_noovfl", "z3 only", true},
    {"bvsmul_noudfl", "z3 only", true},
    {"bvredor", "z3 and Bitwuzla, not Yices", true},
    {"bvredand", "z3 and Bitwuzla, not Yices", true},
    {"const-array", "z3 and Bitwuzla, not Yices", false},
    {"lambda", "z3 only", false},
    {"quantifier", "z3 and Bitwuzla, not Yices", false},
    {"array-map", "z3 only", false},
    {"array-default", "z3 only", false},
    {"as-array", "z3 only", false},
}};

std::optional<Kind> kindOf(const Expr &e) {
  if (isOpX<UMUL_NO_OVERFLOW>(e))
    return K_UMUL_NOOVFL;
  if (isOpX<SMUL_NO_OVERFLOW>(e))
    return K_SMUL_NOOVFL;
  if (isOpX<SMUL_NO_UNDERFLOW>(e))
    return K_SMUL_NOUDFL;
  if (isOpX<BREDOR>(e))
    return K_REDOR;
  if (isOpX<BREDAND>(e))
    return K_REDAND;
  if (isOpX<CONST_ARRAY>(e))
    return K_CONST_ARRAY;
  if (isOpX<LAMBDA>(e))
    return K_LAMBDA;
  if (isOpX<FORALL>(e) || isOpX<EXISTS>(e))
    return K_QUANTIFIER;
  if (isOpX<ARRAY_MAP>(e))
    return K_ARRAY_MAP;
  if (isOpX<ARRAY_DEFAULT>(e))
    return K_ARRAY_DEFAULT;
  if (isOpX<AS_ARRAY>(e))
    return K_AS_ARRAY;
  return std::nullopt;
}

/// Records the constructs seen; with \p expand, rewrites the expandable ones
/// (bottom-up: the arguments are already rewritten) into their exact
/// portable form (checked with z3 for widths 1, 3 and 8: the forms below are
/// equal to z3's predicates for every x, y)
struct PortableRw {
  std::array<bool, K_COUNT> &seen;
  bool expand;
  TypeChecker &tc;

  unsigned widthOf(Expr x) { return bv::width(tc.typeOf(x)); }

  Expr operator()(Expr e) {
    std::optional<Kind> k = kindOf(e);
    if (!k)
      return e;
    seen[*k] = true;
    if (!expand || !kInfo[*k].expandable)
      return e;
    ExprFactory &efac = e->efac();
    Expr x = e->arg(0);
    unsigned w = widthOf(x);
    switch (*k) {
    case K_REDOR:
      // bvredor x = (x != 0)
      return mk<ITE>(mk<EQ>(x, bv::bvnum(0, w, efac)), bv::bvnum(0, 1, efac),
                     bv::bvnum(1, 1, efac));
    case K_REDAND: {
      // bvredand x = (x == ~0)
      mpz_class ones = (mpz_class(1UL) << w) - mpz_class(1UL);
      return mk<ITE>(mk<EQ>(x, bv::bvnum(ones, w, efac)), bv::bvnum(1, 1, efac),
                     bv::bvnum(0, 1, efac));
    }
    case K_UMUL_NOOVFL: {
      // the high half of the 2w-bit product of zero-extended operands is 0
      Expr p = mk<BMUL>(bv::zext(x, 2 * w), bv::zext(e->arg(1), 2 * w));
      return mk<EQ>(bv::extract(2 * w - 1, w, p), bv::bvnum(0, w, efac));
    }
    case K_SMUL_NOOVFL:
    case K_SMUL_NOUDFL: {
      // the 2w-bit product of sign-extended operands, exact, is in
      // [-2^(w-1), 2^(w-1) - 1]: no overflow = at most the upper end, no
      // underflow = at least the lower one
      Expr p = mk<BMUL>(bv::sext(x, 2 * w), bv::sext(e->arg(1), 2 * w));
      mpz_class half = mpz_class(1UL) << (w - 1);
      if (*k == K_SMUL_NOOVFL)
        return mk<BSLE>(p, bv::bvnum(half - mpz_class(1UL), 2 * w, efac));
      mpz_class lo = (mpz_class(1UL) << (2 * w)) - half; // -2^(w-1) mod 2^2w
      return mk<BSGE>(p, bv::bvnum(lo, 2 * w, efac));
    }
    default:
      return e;
    }
  }
};

struct PortableVisitor {
  std::shared_ptr<PortableRw> rw;
  VisitAction operator()(Expr e) {
    return VisitAction::changeDoKidsRewrite(e, rw);
  }
};
} // namespace

std::vector<Smt2Construct> smt2Constructs(ExprVector &asserts,
                                          Smt2Dialect dialect) {
  std::array<bool, K_COUNT> seen{};
  TypeChecker tc;
  bool expand = dialect == Smt2Dialect::Portable;
  PortableVisitor v{std::make_shared<PortableRw>(PortableRw{seen, expand, tc})};
  DagVisit<PortableVisitor> dv(v);
  for (Expr &a : asserts)
    a = dv(a);
  std::vector<Smt2Construct> left;
  for (unsigned k = 0; k < K_COUNT; ++k)
    if (seen[k] && !(expand && kInfo[k].expandable))
      left.push_back({kInfo[k].name, kInfo[k].readers, kInfo[k].expandable});
  return left;
}

void smt2Header(llvm::raw_ostream &o, Smt2Dialect dialect,
                const std::vector<Smt2Construct> &left, llvm::StringRef logic) {
  o << "; sea.smt2-dialect "
    << (dialect == Smt2Dialect::Portable ? "portable" : "z3") << "\n";
  for (const Smt2Construct &c : left) {
    llvm::StringRef fix = c.expandable ? "--smt2-dialect=portable expands it"
                                       : "no portable form";
    o << "; sea.smt2-nonportable " << c.name << ": read by " << c.readers
      << "; " << fix << "\n";
    diag::warn("smt2.nonportable." + c.name, "the SMT-LIB query has " + c.name +
                                                 ", read by " + c.readers +
                                                 "; " + fix);
  }
  o << "(set-logic " << logic << ")\n";
}

} // namespace solver
} // namespace sea
