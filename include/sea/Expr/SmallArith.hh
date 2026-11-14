// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/// Modular integer helpers of the rewriting rules (SeaM offsets, the
/// bit-vector normal form) with a small-integer fast path.
///
/// Each function computes the same mpz value as its plain GMP definition
/// (stated with it) for every input. When the operands fit in int64_t and the
/// width is at most 64, the result comes from 64-bit arithmetic (signed
/// steps checked with __builtin_*_overflow, modular steps in uint64_t, exact
/// mod 2^64); otherwise, or on overflow, from the GMP definition. The fast
/// path saves GMP's temporaries (2^w, the quotient, the sum), not the result.
/// unittests/SmallArith checks both paths against the GMP definitions at the
/// 64-bit boundaries.
#pragma once

#include "sea/Expr/ExprGmp.hh"

#include <cstdint>

namespace expr {
namespace smallarith {

static_assert(sizeof(long) == sizeof(int64_t),
              "the fast path reads mpz values through mpz_get_si (LP64)");

/// \p v as int64_t when it fits.
inline bool toI64(const mpz_class &v, int64_t &out) {
  if (!mpz_fits_slong_p(v.get_mpz_t()))
    return false;
  out = mpz_get_si(v.get_mpz_t());
  return true;
}

/// 2^w - 1 for w <= 64 (the residues mod 2^w as bits).
inline uint64_t mask(unsigned w) { return w >= 64 ? ~0ULL : (1ULL << w) - 1; }

/// The GMP definitions (the reference of the fast paths, and their fallback).
namespace ref {
inline mpz_class modPow2(const mpz_class &v, unsigned w) {
  mpz_class m, r;
  mpz_ui_pow_ui(m.get_mpz_t(), 2, w);
  mpz_fdiv_r(r.get_mpz_t(), v.get_mpz_t(), m.get_mpz_t());
  return r;
}
inline mpz_class modPow2Add(const mpz_class &a, const mpz_class &b,
                            unsigned w) {
  mpz_class s;
  mpz_add(s.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
  return modPow2(s, w);
}
inline mpz_class modPow2Sub(const mpz_class &a, const mpz_class &b,
                            unsigned w) {
  mpz_class s;
  mpz_sub(s.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
  return modPow2(s, w);
}
} // namespace ref

/// v mod 2^w, in [0, 2^w) (floor remainder: mpz_fdiv_r by 2^w).
inline mpz_class modPow2(const mpz_class &v, unsigned w) {
  int64_t x;
  if (w <= 64 && toI64(v, x))
    // two's complement: the low w bits of x are x mod 2^w
    return mpz_class((unsigned long)((uint64_t)x & mask(w)));
  return ref::modPow2(v, w);
}

/// (a + b) mod 2^w.
inline mpz_class modPow2Add(const mpz_class &a, const mpz_class &b,
                            unsigned w) {
  int64_t x, y;
  if (w <= 64 && toI64(a, x) && toI64(b, y))
    // addition mod 2^64 is exact on the residues; mod 2^w then takes bits
    return mpz_class((unsigned long)(((uint64_t)x + (uint64_t)y) & mask(w)));
  return ref::modPow2Add(a, b, w);
}

/// (a - b) mod 2^w.
inline mpz_class modPow2Sub(const mpz_class &a, const mpz_class &b,
                            unsigned w) {
  int64_t x, y;
  if (w <= 64 && toI64(a, x) && toI64(b, y))
    return mpz_class((unsigned long)(((uint64_t)x - (uint64_t)y) & mask(w)));
  return ref::modPow2Sub(a, b, w);
}

/// acc += v, in place (no temporary).
inline void addTo(mpz_class &acc, const mpz_class &v) {
  mpz_add(acc.get_mpz_t(), acc.get_mpz_t(), v.get_mpz_t());
}

/// The circular range test (seam::Rules::circularIn): the residues mod 2^w
/// of the integers [lo, hi] form one arc [l, l + (hi - lo)] with l = lo mod
/// 2^w; -1 when that arc is empty (hi < lo), covers the circle (hi - lo >=
/// 2^w) or passes 2^w - 1; else 1 when its top is < nlo, 2 when its bottom
/// is >= nhi, 0 otherwise. GMP definition:
///   span = hi - lo; if span < 0 or span >= 2^w: -1
///   l = lo mod 2^w; h = l + span; if h >= 2^w: -1
///   h < nlo: 1; l >= nhi: 2; else 0
namespace ref {
inline int circular(const mpz_class &lo, const mpz_class &hi,
                    const mpz_class &nlo, const mpz_class &nhi, unsigned w) {
  mpz_class mod;
  mpz_ui_pow_ui(mod.get_mpz_t(), 2, w);
  mpz_class span = hi - lo;
  if (span < mpz_class(0UL) || span >= mod)
    return -1;
  mpz_class l = modPow2(lo, w), h = l + span;
  if (h >= mod)
    return -1;
  if (h < nlo)
    return 1;
  if (l >= nhi)
    return 2;
  return 0;
}
} // namespace ref

inline int circular(const mpz_class &lo, const mpz_class &hi,
                    const mpz_class &nlo, const mpz_class &nhi, unsigned w) {
  int64_t a, b, n0, n1, span;
  if (w > 64 || !toI64(lo, a) || !toI64(hi, b) || !toI64(nlo, n0) ||
      !toI64(nhi, n1) || __builtin_sub_overflow(b, a, &span))
    return ref::circular(lo, hi, nlo, nhi, w);
  const uint64_t m = mask(w);
  // span < 2^w <=> span <= 2^w - 1 (span >= 0 here)
  if (span < 0 || (uint64_t)span > m)
    return -1;
  uint64_t l = (uint64_t)a & m, h;
  if (__builtin_add_overflow(l, (uint64_t)span, &h) || h > m)
    return -1; // h >= 2^w (an overflow of uint64_t is h >= 2^64 >= 2^w)
  // compares of an unsigned residue with a signed bound
  if (n0 > 0 && h < (uint64_t)n0)
    return 1;
  if (n1 <= 0 || l >= (uint64_t)n1)
    return 2;
  return 0;
}

} // namespace smallarith
} // namespace expr
