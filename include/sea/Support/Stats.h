// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Stats.h - stat lines and their BRUNCH_STAT form ----------*- C++ -*-===//
//
// The `key: value` stat lines of a tool and their `BRUNCH_STAT <key>
// <value>` lines (--brunch-stats), from one description:
//
//  Line    one stat line: a key and the tokens of its value (value, kv,
//          note, seconds, text), and how it becomes BRUNCH_STAT lines.
//  Report  the lines of one block, in order. print() writes the text
//          lines, then (on request) the BRUNCH_STAT lines; both come from
//          one function (renderLine), so they cannot drift.
//  Brunch  how a line becomes BRUNCH_STAT lines:
//            Off      none
//            Value    `BRUNCH_STAT <key> <value>`, the whole value
//            Parts    a leading state word: `<key> <state>`; each k=v
//                     token: `<key>-<k> <v>`; parenthesised notes left out
//            Derived  the rule of sea-vc's SeaM block: Parts for seam.inb
//                     and assume.*, else Value when the value is one
//                     token, else none
//
// A report is pulled from the components of one run (their report()
// methods): there is no global registry, so a retry or a split part has
// its own.
//
// memFields: the one source of the rss-mb / vm-mb / peak-rss-mb fields of
// a ProcMem reading (progress and phase events).
//
//===----------------------------------------------------------------------===//

#ifndef SEA_SUPPORT_STATS_H
#define SEA_SUPPORT_STATS_H

#include "sea/Support/ProcMem.h"
#include "sea/Support/Timer.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace sea {

/// The memory fields of \p m, in their printed order: rss-mb vm-mb (when
/// known), peak-rss-mb (when known); values in MiB. Does not allocate.
llvm::SmallVector<std::pair<llvm::StringRef, uint64_t>, 3>
memFields(const ProcMem &m);

namespace stats {

/// How a line becomes BRUNCH_STAT lines (see the file comment).
enum class Brunch { Off, Value, Parts, Derived };

/// One stat line (see the file comment). Made by Report::line; the
/// builders append to the value.
class Line {
public:
  /// Appends ` <v>` (any type raw_ostream prints).
  template <typename T>
  Line &value(const T &v) {
    llvm::raw_string_ostream(val) << ' ' << v;
    return *this;
  }
  /// Appends ` <seconds>` in the printed form of a duration (secondsText).
  Line &seconds(double s);
  /// Appends ` <k>=<v>`.
  template <typename T>
  Line &kv(llvm::StringRef k, const T &v) {
    llvm::raw_string_ostream(val) << ' ' << k << '=' << v;
    return *this;
  }
  /// Appends ` (<t>)`: a note (Parts leaves it out).
  template <typename T>
  Line &note(const T &t) {
    llvm::raw_string_ostream(val) << " (" << t << ')';
    return *this;
  }
  /// Appends \p t verbatim (the caller supplies the separators).
  Line &text(llvm::StringRef t) {
    val += t;
    return *this;
  }
  /// The BRUNCH_STAT rule of this line (default: the report's).
  Line &brunch(Brunch b) {
    policy = b;
    return *this;
  }
  /// Prints only the BRUNCH_STAT lines of this line, no text line.
  Line &brunchOnly() {
    textOff = true;
    return *this;
  }

  llvm::StringRef key() const { return k; }
  /// The value as printed after `key:` (empty, or starting with a blank).
  llvm::StringRef valueText() const { return val; }
  Brunch brunchPolicy() const { return policy; }
  bool isBrunchOnly() const { return textOff; }

private:
  friend class Report;
  Line(std::string key, Brunch b) : k(std::move(key)), policy(b) {}
  std::string k, val;
  Brunch policy;
  bool textOff = false;
};

/// The stat lines of one block (see the file comment).
class Report {
public:
  /// \p brunch: the BRUNCH_STAT rule of the lines (Line::brunch overrides).
  explicit Report(Brunch brunch = Brunch::Off) : policy(brunch) {}
  Report(const Report &) = delete;
  Report &operator=(const Report &) = delete;

  /// A new line \p key, after the others. The reference stays valid for
  /// the life of the report.
  Line &line(llvm::StringRef key);
  /// `<timer name>-time: <seconds>`.
  Line &time(const Timer &t);
  /// One line per timer of \p g, in first-use order: `<g.key(name)>:
  /// <seconds>`.
  void timers(const TimerGroup &g);

  /// Writes the text lines, then, with \p brunch, the BRUNCH_STAT lines.
  void print(llvm::raw_ostream &os, bool brunch) const;
  /// The text lines (print without BRUNCH_STAT lines).
  std::string text() const;

  const std::deque<Line> &lines() const { return all; }
  bool empty() const { return all.empty(); }

private:
  Brunch policy;
  /// deque: references stay valid as lines are added
  std::deque<Line> all;
};

} // namespace stats
} // namespace sea

#endif // SEA_SUPPORT_STATS_H
