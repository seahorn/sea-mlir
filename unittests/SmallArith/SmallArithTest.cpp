// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- SmallArithTest.cpp - the small-integer fast paths ------------------===//
//
// Plain-main driver (no gtest): prints `test: <name> ok` lines and
// `failures: N`, exits non-zero if a check fails. Run by
// test/Expr/small-arith.test.
//
// What is tested: every function of include/sea/Expr/SmallArith.hh against
// its GMP definition (smallarith::ref, restated here with plain mpz calls)
// on all combinations of boundary values: 0, +-1, small values, INT64_MIN,
// INT64_MAX and their neighbours, 2^63 and 2^64 (+-1), values beyond 64
// bits; widths 0, 1, 7, 8, 31, 32, 62, 63, 64, 65 and 128 (the fast path
// takes w <= 64 and int64 operands; the others take the GMP path). Both
// paths must give the same value.
//
//===----------------------------------------------------------------------===//

#include "sea/Expr/SmallArith.hh"

#include "llvm/Support/raw_ostream.h"

#include <cstdint>
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

mpz_class pow2(unsigned k) {
  mpz_class r;
  mpz_ui_pow_ui(r.get_mpz_t(), 2, k);
  return r;
}

/// The reference: plain mpz, written independently of SmallArith.hh.
mpz_class refMod(const mpz_class &v, unsigned w) {
  mpz_class r;
  mpz_fdiv_r(r.get_mpz_t(), v.get_mpz_t(), pow2(w).get_mpz_t());
  return r;
}

std::vector<mpz_class> values() {
  std::vector<mpz_class> vs;
  for (long x : {0L, 1L, -1L, 2L, -2L, 7L, -7L, 255L, -256L, 4096L})
    vs.push_back(mpz_class(x));
  vs.push_back(mpz_class((long)INT64_MAX));
  vs.push_back(mpz_class((long)(INT64_MAX - 1)));
  vs.push_back(mpz_class((long)INT64_MIN));
  vs.push_back(mpz_class((long)(INT64_MIN + 1)));
  for (unsigned k : {31u, 32u, 62u, 63u, 64u, 65u, 100u, 128u}) {
    mpz_class p = pow2(k);
    vs.push_back(p);
    vs.push_back(p - mpz_class(1L));
    vs.push_back(p + mpz_class(1L));
    vs.push_back(mpz_class(0L) - p);
    vs.push_back(mpz_class(0L) - p + mpz_class(1L));
    vs.push_back(mpz_class(0L) - p - mpz_class(1L));
  }
  return vs;
}

const unsigned widths[] = {0, 1, 7, 8, 31, 32, 62, 63, 64, 65, 128};

std::string str(const mpz_class &v) { return v.to_string(); }

void testModPow2() {
  int before = failures;
  for (const mpz_class &v : values())
    for (unsigned w : widths) {
      mpz_class got = smallarith::modPow2(v, w), want = refMod(v, w);
      CHECK(got == want,
            "modPow2(" << str(v) << ", " << w << ") = " << str(got));
      CHECK(smallarith::ref::modPow2(v, w) == want,
            "ref::modPow2(" << str(v) << ", " << w << ")");
    }
  llvm::outs() << "test: mod-pow2 " << (failures == before ? "ok" : "FAIL")
               << "\n";
}

void testAddSub() {
  int before = failures;
  std::vector<mpz_class> vs = values();
  for (const mpz_class &a : vs)
    for (const mpz_class &b : vs)
      for (unsigned w : widths) {
        mpz_class s, d;
        mpz_add(s.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
        mpz_sub(d.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
        CHECK(smallarith::modPow2Add(a, b, w) == refMod(s, w),
              "modPow2Add(" << str(a) << ", " << str(b) << ", " << w << ")");
        CHECK(smallarith::modPow2Sub(a, b, w) == refMod(d, w),
              "modPow2Sub(" << str(a) << ", " << str(b) << ", " << w << ")");
        mpz_class acc = a;
        smallarith::addTo(acc, b);
        CHECK(acc == s, "addTo(" << str(a) << ", " << str(b) << ")");
      }
  llvm::outs() << "test: add-sub " << (failures == before ? "ok" : "FAIL")
               << "\n";
}

/// The circular test as specified in SmallArith.hh, restated over mpz.
int refCircular(const mpz_class &lo, const mpz_class &hi, const mpz_class &nlo,
                const mpz_class &nhi, unsigned w) {
  mpz_class mod = pow2(w), span;
  mpz_sub(span.get_mpz_t(), hi.get_mpz_t(), lo.get_mpz_t());
  if (span.sgn() < 0 || span >= mod)
    return -1;
  mpz_class l = refMod(lo, w), h;
  mpz_add(h.get_mpz_t(), l.get_mpz_t(), span.get_mpz_t());
  if (h >= mod)
    return -1;
  if (h < nlo)
    return 1;
  if (l >= nhi)
    return 2;
  return 0;
}

void testCircular() {
  int before = failures;
  // fewer values: four operands
  std::vector<mpz_class> vs;
  for (long x : {0L, 1L, -1L, 7L, 4096L})
    vs.push_back(mpz_class(x));
  vs.push_back(mpz_class((long)INT64_MAX));
  vs.push_back(mpz_class((long)INT64_MIN));
  vs.push_back(mpz_class((long)(INT64_MIN + 1)));
  for (unsigned k : {32u, 63u, 64u, 65u}) {
    vs.push_back(pow2(k) - mpz_class(1L));
    vs.push_back(pow2(k));
    vs.push_back(mpz_class(0L) - pow2(k));
  }
  const unsigned ws[] = {0, 1, 8, 32, 62, 63, 64, 65};
  uint64_t cases = 0, answers[4] = {0, 0, 0, 0};
  for (const mpz_class &lo : vs)
    for (const mpz_class &hi : vs)
      for (const mpz_class &nlo : vs)
        for (const mpz_class &nhi : vs)
          for (unsigned w : ws) {
            int want = refCircular(lo, hi, nlo, nhi, w);
            ++cases;
            ++answers[want + 1];
            CHECK(smallarith::circular(lo, hi, nlo, nhi, w) == want,
                  "circular(" << str(lo) << ", " << str(hi) << ", " << str(nlo)
                              << ", " << str(nhi) << ", " << w << ")");
            CHECK(smallarith::ref::circular(lo, hi, nlo, nhi, w) == want,
                  "ref::circular(" << str(lo) << ", " << str(hi) << ", "
                                   << str(nlo) << ", " << str(nhi) << ", " << w
                                   << ")");
          }
  // every answer (wrap / empty, undecided, In, Out) is reached
  for (uint64_t n : answers)
    CHECK(n > 0, "an answer never reached");
  CHECK(cases > 100000, "case count");
  llvm::outs() << "test: circular " << (failures == before ? "ok" : "FAIL")
               << "\n";
}

} // namespace

int main() {
  testModPow2();
  testAddSub();
  testCircular();
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
