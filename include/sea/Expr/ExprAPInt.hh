// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/// Expr <-> LLVM support types: printing to raw_ostream and APInt <-> mpz.
///
/// Replaces SeaHorn's ExprLlvm.hh. sea-mlir uses Expr for SMT terms only, so
/// the llvm::Value / BasicBlock / Function terminals are gone; only the
/// helpers that do not depend on LLVM IR are kept.
#pragma once

#include "sea/Expr/Expr.hh"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

// ExprLlvm.hh provided boost::hash for Expr op kinds; code relies on it.
#include <boost/functional/hash.hpp>
#include <boost/lexical_cast.hpp>
#include <cstdlib>

namespace expr {
// SeaHorn code that included ExprLlvm.hh relies on this.
using namespace llvm;

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Expr &p) {
  OS << p.get();
  return OS;
}

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const ENode &n) {
  OS << boost::lexical_cast<std::string>(n);
  return OS;
}

/** Converts v to ::mpz_class. Assumes that v is signed */
inline expr::mpz_class toMpz(const llvm::APInt &v) {
  // Based on:
  // https://llvm.org/svn/llvm-project/polly/trunk/lib/Support/GICHelper.cpp
  llvm::APInt abs;
  abs = v.isNegative() ? v.abs() : v;

  const uint64_t *rawdata = abs.getRawData();
  unsigned numWords = abs.getNumWords();

  expr::mpz_class res;
  mpz_import(res.get_mpz_t(), numWords, -1, sizeof(uint64_t), 0, 0, rawdata);
  if (v.isNegative())
    res.neg();
  return res;
}

/** Adapted from
    https://llvm.org/svn/llvm-project/polly/branches/release_34/lib/Support/GICHelper.cpp
*/
inline llvm::APInt toAPInt(const expr::mpz_class &v) {
  uint64_t *p = nullptr;
  size_t sz;

  p = (uint64_t *)mpz_export(p, &sz, -1, sizeof(uint64_t), 0, 0, v.get_mpz_t());
  if (p) {
    llvm::APInt A((unsigned)mpz_sizeinbase(v.get_mpz_t(), 2),
                  llvm::ArrayRef<uint64_t>(p, sz));
    A = A.zext(A.getBitWidth() + 1);
    free(p);

    if (v.sgn() == -1)
      return -A;
    else
      return A;
  } else
    return llvm::APInt(1, 0);
}

inline llvm::APInt toAPInt(unsigned numBits, const expr::mpz_class &v) {
  uint64_t *p = nullptr;
  size_t sz;

  p = (uint64_t *)mpz_export(p, &sz, -1, sizeof(uint64_t), 0, 0, v.get_mpz_t());
  if (p) {
    // mpz_export returns at least one word; a value wider than numBits is
    // truncated (it is taken modulo 2^numBits).
    llvm::APInt A(std::max<unsigned>(numBits, sz * 64),
                  llvm::ArrayRef<uint64_t>(p, sz));
    A = A.trunc(numBits);
    free(p);
    if (v.sgn() == -1)
      return -A;
    else
      return A;
  } else
    return llvm::APInt(numBits, 0);
}
} // namespace expr
