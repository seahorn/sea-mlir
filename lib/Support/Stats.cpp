// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Stats.cpp - stat lines and their BRUNCH_STAT form ------------------===//

#include "sea/Support/Stats.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"

using namespace sea;
using namespace sea::stats;

llvm::SmallVector<std::pair<llvm::StringRef, uint64_t>, 3>
sea::memFields(const ProcMem &m) {
  llvm::SmallVector<std::pair<llvm::StringRef, uint64_t>, 3> f;
  if (m.hasCurrent) {
    f.emplace_back("rss-mb", ProcMem::mb(m.rssKB));
    f.emplace_back("vm-mb", ProcMem::mb(m.vmKB));
  }
  if (m.hasPeak)
    f.emplace_back("peak-rss-mb", ProcMem::mb(m.peakRssKB));
  return f;
}

Line &Line::seconds(double s) {
  val += ' ';
  val += secondsText(s);
  return *this;
}

Line &Report::line(llvm::StringRef key) {
  all.push_back(Line(key.str(), policy));
  return all.back();
}

void Report::append(const Report &o) {
  for (const Line &l : o.all)
    all.push_back(l);
}

Line &Report::time(const Timer &t) {
  return line((t.name() + "-time").str()).seconds(t.seconds());
}

void Report::timers(const TimerGroup &g) {
  g.report([&](llvm::StringRef key, const Timer &t) {
    line(key).seconds(t.seconds());
  });
}

namespace {

void brunchStat(llvm::raw_ostream &os, const llvm::Twine &key,
                const llvm::Twine &value) {
  os << "BRUNCH_STAT " << key << " " << value << "\n";
}

/// Parts: a leading state word (assumed / off / vc), then the k=v tokens:
/// `<key> <state>`, `<key>-<k> <v>` (notes in parentheses are left out)
void brunchParts(llvm::raw_ostream &os, llvm::StringRef key,
                 llvm::StringRef value) {
  llvm::SmallVector<llvm::StringRef> words;
  value.split(words, ' ', -1, /*KeepEmpty=*/false);
  bool inNote = false; // a parenthesised note: left out whole
  for (auto [i, w] : llvm::enumerate(words)) {
    if (inNote || w.starts_with("(")) {
      inNote = !w.ends_with(")");
      continue;
    }
    auto [k, v] = w.split('=');
    if (!v.empty())
      brunchStat(os, key + "-" + k, v);
    else if (i == 0)
      brunchStat(os, key, w);
  }
}

enum class Pass { Text, Brunch };

/// The one rendering of a line: its text line (Pass::Text) or its
/// BRUNCH_STAT lines (Pass::Brunch), both from the text `key:value`.
void renderLine(const Line &l, Pass pass, llvm::raw_ostream &os) {
  std::string text = (l.key() + ":" + l.valueText()).str();
  if (pass == Pass::Text) {
    if (!l.isBrunchOnly())
      os << text << "\n";
    return;
  }
  llvm::StringRef value = l.valueText();
  value.consume_front(" ");
  switch (l.brunchPolicy()) {
  case Brunch::Off:
    return;
  case Brunch::Value:
    brunchStat(os, l.key(), value);
    return;
  case Brunch::Parts:
    brunchParts(os, l.key(), value);
    return;
  case Brunch::Derived: {
    // sea-vc's SeaM block: every `key: value` line with a one-token value,
    // plus the parts of seam.inb and assume.*
    auto [key, val] = llvm::StringRef(text).split(": ");
    if (key == "seam.inb" || key.starts_with("assume."))
      brunchParts(os, key, val);
    else if (!val.empty() && !val.contains(' '))
      brunchStat(os, key, val);
    return;
  }
  }
}

} // namespace

void Report::print(llvm::raw_ostream &os, bool brunch) const {
  for (const Line &l : all)
    renderLine(l, Pass::Text, os);
  if (!brunch)
    return;
  for (const Line &l : all)
    renderLine(l, Pass::Brunch, os);
}

void Report::printEach(llvm::raw_ostream &os, bool brunch) const {
  for (const Line &l : all) {
    renderLine(l, Pass::Text, os);
    if (brunch)
      renderLine(l, Pass::Brunch, os);
  }
}

std::string Report::text() const {
  std::string s;
  llvm::raw_string_ostream os(s);
  print(os, false);
  return s;
}
