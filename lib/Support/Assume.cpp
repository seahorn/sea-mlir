// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Assume.cpp - the ledger of named assumptions -----------------------===//

#include "sea/Support/Assume.h"

#include "sea/Support/Stats.h"

using namespace sea;
using namespace sea::assume;

namespace {
struct Info {
  const char *name;
  bool byRuns;
};
// in Name order
constexpr Info kInfo[kNames] = {
    {"memset-bound", false},
    {"a-inb", false},
    {"addr0-is-null", true},
    {"memhavoc-null-noop", true},
};
} // namespace

llvm::StringRef sea::assume::nameOf(Name a) {
  return kInfo[static_cast<unsigned>(a)].name;
}

bool sea::assume::countedByRuns(Name a) {
  return kInfo[static_cast<unsigned>(a)].byRuns;
}

void Ledger::use(Name a, uint64_t n) {
  Entry &e = at(a);
  e.uses += n;
  e.runs += n != 0;
}

void Ledger::site(Name a, llvm::StringRef field, uint64_t n) {
  Entry &e = at(a);
  e.hasSites = true;
  e.sites.emplace_back(field.str(), n);
}

void Ledger::add(const Ledger &o) {
  for (unsigned i = 0; i < kNames; ++i) {
    all[i].uses += o.all[i].uses;
    all[i].runs += o.all[i].runs;
    all[i].hasSites |= o.all[i].hasSites;
    all[i].sites.append(o.all[i].sites.begin(), o.all[i].sites.end());
  }
}

bool Ledger::anyUse() const {
  for (const Entry &e : all)
    if (e.uses)
      return true;
  return false;
}

void Ledger::report(stats::Report &r) const {
  // the report's rule decides whether there are BRUNCH_STAT lines; their
  // form is the ledger's
  auto brunch = r.brunchPolicy() == stats::Brunch::Off ? stats::Brunch::Off
                                                       : stats::Brunch::Parts;
  for (unsigned i = 0; i < kNames; ++i) {
    Name a = static_cast<Name>(i);
    const Entry &e = all[i];
    std::string key = ("assume." + nameOf(a)).str();
    if (e.hasSites) {
      stats::Line &l = r.line(key).brunch(brunch);
      if (countedByRuns(a))
        l.value("vc");
      for (const auto &[k, v] : e.sites)
        l.kv(k, v);
    }
    if (e.uses)
      r.line(key).brunch(brunch).kv("uses", e.uses);
  }
}

void Ledger::runFields(llvm::raw_ostream &os, bool withRuns) const {
  const char *sep = "";
  for (unsigned i = 0; i < kNames; ++i) {
    Name a = static_cast<Name>(i);
    if (!countedByRuns(a))
      continue;
    os << sep << nameOf(a) << "=" << all[i].uses;
    if (withRuns)
      os << " " << nameOf(a) << "-runs=" << all[i].runs;
    sep = " ";
  }
}

std::string Ledger::runFields(bool withRuns) const {
  std::string s;
  llvm::raw_string_ostream os(s);
  runFields(os, withRuns);
  return s;
}
