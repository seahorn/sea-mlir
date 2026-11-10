// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Log.h - tagged, leveled logs on stderr -------------------*- C++ -*-===//
//
// A log point belongs to a tag, declared once at namespace scope with a
// name, the highest level it emits and a one-line description:
//
//   static sea::log::Tag logStore("vc.store-read", 3, "havoc of a read key");
//
// and emits an event through SEA_LOG, which evaluates its arguments only
// when the tag is on at that level:
//
//   SEA_LOG(logStore, 3).msg("read").kv("uses", n).at(op->getLoc());
//
// Levels are granularity, not severity: the level is the innermost loop
// around the log point (1 flow: once per phase or pass; 2 object: per
// function, memory object, assert, load; 3 detail: per op). Nothing finer
// than per-op is a log point at any level.
//
// Selection (docs/reference/logging.md): SEA_LOG (environment) first, then
// each --log, comma-separated `pattern[:level]` (pattern = a tag, a prefix
// `a.b.*`, or `*`; level 1 by default, 0 = off); a later entry wins per tag.
// `--log=help` lists the tags. Events go to stderr, or to --log-file /
// SEA_LOG_FILE (appended; opened at the first event), one whole line each:
//
//   log: <tag> [<msg>] k=v ... [at <loc>]
//
// or, after setFormat(Jsonl), one JSON object per line:
//
//   {"log":"<tag>","level":L,"msg":..,<k>:<v>...,"loc":..,"text":"<line>"}
//
// The keys log, level, msg, loc and text are reserved: kv asserts that it
// is not given one.
// The guard is one load and compare (Tag::on); with NSEALOG defined it is
// constant false and every log point compiles out. The --log, --log-file
// options and the warning selection (warningSelected) stay.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_SUPPORT_LOG_H
#define SEA_SUPPORT_LOG_H

#include "sea/Support/Output.h"

#include "mlir/IR/Location.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <cassert>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sea::log {

/// Granularity levels (see the file comment).
enum : unsigned { Flow = 1, Object = 2, Detail = 3, MaxLevel = 3 };

class Tag {
public:
  /// Registers the tag; its level is set from the selection made so far
  /// and updated by later --log entries. A name declared twice is a fatal
  /// error at startup.
  Tag(const char *name, unsigned maxLevel, const char *description);
  ~Tag();
  Tag(const Tag &) = delete;
  Tag &operator=(const Tag &) = delete;

  /// True if events of `level` (1..maxLevel) are selected.
  bool on(unsigned level) const {
    assert(level >= 1 && level <= maxLvl && "log level above the tag's max");
#ifdef NSEALOG
    (void)level;
    return false;
#else
    return lvl.load(std::memory_order_relaxed) >= level;
#endif
  }

  llvm::StringRef name() const { return nm; }
  unsigned maxLevel() const { return maxLvl; }
  llvm::StringRef description() const { return desc; }
  /// The selected level (0 = off).
  unsigned level() const { return lvl.load(std::memory_order_relaxed); }

private:
  friend struct Registry;
  const char *nm;
  const char *desc;
  unsigned maxLvl;
  std::atomic<unsigned char> lvl{0};
};

/// One event: built by SEA_LOG, written whole when it goes out of scope (at
/// the end of the statement).
class Event {
public:
  Event(const Tag &tag, unsigned level) : tag(tag), level(level) {}
  ~Event();
  Event(const Event &) = delete;
  Event &operator=(const Event &) = delete;

  /// Free text after the tag (newlines escaped).
  Event &msg(const llvm::Twine &m);
  /// The location printed at the end (`at <loc>`; "loc" in jsonl).
  Event &at(mlir::Location loc);
  /// A field `key=value`: integers stay numbers, bool is true/false,
  /// strings are bare when they are one token, else quoted; anything else
  /// is printed with raw_ostream <<.
  template <typename T>
  Event &kv(llvm::StringRef key, const T &v) {
    if constexpr (std::is_same_v<T, bool>)
      return field(key, v ? "true" : "false", v ? "true" : "false");
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
      return num(key, int64_t(v));
    else if constexpr (std::is_integral_v<T>)
      return unum(key, uint64_t(v));
    else if constexpr (std::is_floating_point_v<T>)
      return real(key, double(v));
    else if constexpr (std::is_convertible_v<const T &, llvm::StringRef>)
      return str(key, llvm::StringRef(v));
    else {
      std::string s;
      llvm::raw_string_ostream os(s);
      os << v;
      return str(key, s);
    }
  }

private:
  Event &num(llvm::StringRef key, int64_t v);
  Event &unum(llvm::StringRef key, uint64_t v);
  Event &real(llvm::StringRef key, double v);
  Event &str(llvm::StringRef key, llvm::StringRef v);
  Event &field(llvm::StringRef key, std::string text, std::string json);

  const Tag &tag;
  unsigned level;
  std::string message;
  std::string loc;
  /// key, text rendering, JSON rendering
  struct Field {
    std::string key, text, json;
  };
  std::vector<Field> fields;
};

/// Reads SEA_LOG and SEA_LOG_FILE (once). Tools call it at the start of
/// main, before parsing options: --log entries then override it. Warns
/// about entries of SEA_LOG that match no tag (log.unknown-tag).
void init();

/// The format of events: text (default) or, under a tool's --format=jsonl,
/// one JSON object per line.
void setFormat(sea::out::Format f);

/// Whether every occurrence of warning `kind` prints (sea::diag::warn):
/// warn.<kind> selected at level 2 or more, or the old spelling, a bare
/// `<kind>` entry at level 1 or more.
bool warningSelected(llvm::StringRef kind);

// -- the selection syntax and the text rendering (exposed for tests) -------

struct Selector {
  std::string pattern;
  unsigned level = 1;
};

/// Parses comma-separated `pattern[:level]` entries into `out`. Empty
/// entries are skipped; a malformed entry is skipped with one message in
/// `errs`. Returns true if no entry was malformed.
bool parseSpec(llvm::StringRef spec, std::vector<Selector> &out,
               std::vector<std::string> &errs);

/// A string value as the text format prints it: bare when it is one
/// printable token, else quoted with \" \\ \n \r \t and \xNN (other
/// control bytes) escaped.
std::string textValue(llvm::StringRef v);

/// Whether `pattern` (a name, `prefix.*`, or `*`) selects `name`.
/// `a.*` selects `a` and every `a.<rest>`.
bool matches(llvm::StringRef pattern, llvm::StringRef name);

/// The level the selection gives `name`: the last matching entry, else 0.
unsigned levelOf(llvm::ArrayRef<Selector> sels, llvm::StringRef name);

} // namespace sea::log

namespace sea::log {
/// Ends SEA_LOG's conditional expression (glog's Voidify).
struct Voidify {
  void operator&(const Event &) {}
};
} // namespace sea::log

/// One log event of TAG at LEVEL; the builder calls after it are evaluated
/// only when the tag is on at LEVEL. An expression: no dangling else.
#ifndef NSEALOG
#define SEA_LOG(TAG, LEVEL)                                                    \
  !(TAG).on(LEVEL) ? (void)0                                                   \
                   : ::sea::log::Voidify() & ::sea::log::Event((TAG), (LEVEL))
#else
#define SEA_LOG(TAG, LEVEL)                                                    \
  true ? (void)0 : ::sea::log::Voidify() & ::sea::log::Event((TAG), (LEVEL))
#endif

#endif // SEA_SUPPORT_LOG_H
