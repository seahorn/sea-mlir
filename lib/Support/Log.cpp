// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Log.cpp - tagged, leveled logs on stderr -----------------*- C++ -*-===//

#include "sea/Support/Log.h"
#include "sea/Support/Diag.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>

using namespace llvm;

namespace sea::log {

struct Registry {
  /// Guards tags and sels (registration, --log entries, warningSelected).
  std::mutex m;
  std::vector<Tag *> tags;
  /// SEA_LOG's entries, then each --log's, in order: the last match wins.
  std::vector<Selector> sels;
  bool envRead = false;
  /// Guards the output: one whole line per event.
  std::mutex outLock;
  out::Format format = out::Format::Text;
  /// --log-file / SEA_LOG_FILE: opened at the first event (nothing is
  /// created when no event fires); empty: stderr
  std::string filePath;
  std::string fileSource;
  bool fileTried = false;
  std::unique_ptr<raw_fd_ostream> file;

  /// The stream of events. Caller holds outLock.
  raw_ostream &sink() {
    if (!fileTried && !filePath.empty()) {
      fileTried = true;
      std::error_code ec;
      file = std::make_unique<raw_fd_ostream>(filePath, ec, sys::fs::OF_Append);
      if (ec) {
        file.reset();
        diag::warn("log.file", fileSource + ": cannot open '" + filePath +
                                   "': " + ec.message() +
                                   "; logs go to stderr");
      }
    }
    return file ? static_cast<raw_ostream &>(*file) : errs();
  }

  static Registry &get() {
    static Registry r;
    return r;
  }

  /// Applies `s` to every registered tag it selects. Caller holds m.
  void apply(const Selector &s) {
    for (Tag *t : tags)
      if (matches(s.pattern, t->nm))
        t->lvl.store(s.level, std::memory_order_relaxed);
  }

  /// Whether a pattern needs a registered tag to be meaningful: `*` and the
  /// warning prefix do not. Caller holds m.
  bool known(StringRef pattern) {
    if (pattern == "*" || pattern.starts_with("warn."))
      return true;
    return llvm::any_of(tags, [&](Tag *t) { return matches(pattern, t->nm); });
  }
};

namespace {

/// Adds the entries of `spec` (from `source`: SEA_LOG or --log); warns
/// about malformed entries and entries that select no tag.
void addSpec(StringRef spec, StringRef source) {
  std::vector<Selector> sels;
  std::vector<std::string> errs;
  parseSpec(spec, sels, errs);
  std::vector<std::string> unknown;
  {
    Registry &r = Registry::get();
    std::lock_guard<std::mutex> g(r.m);
    for (Selector &s : sels) {
      if (!r.known(s.pattern))
        unknown.push_back(s.pattern);
      r.apply(s);
      r.sels.push_back(std::move(s));
    }
  }
  for (const std::string &e : errs)
    diag::warn("log.bad-spec", source + ": " + e + " (ignored)");
  for (const std::string &p : unknown)
    diag::warn("log.unknown-tag",
               source + ": no log tag matches '" + p +
                   "' (--log=help lists them); if '" + p +
                   "' is a warning kind, every occurrence prints: the old "
                   "spelling of --log=warn." +
                   p + ":2");
}

/// Sets the log file; it is opened at the first event. A later call (the
/// flag after the environment) overrides an earlier one.
void setFile(StringRef path, StringRef source) {
  Registry &r = Registry::get();
  std::lock_guard<std::mutex> g(r.outLock);
  r.filePath = path.str();
  r.fileSource = source.str();
  r.fileTried = false;
  r.file.reset();
}

/// Whether `path` can be opened for appending, without creating it: the
/// file is writable, or it does not exist and its directory is.
bool writable(StringRef path) {
  if (sys::fs::exists(path))
    return !sys::fs::is_directory(path) &&
           !sys::fs::access(path, sys::fs::AccessMode::Write);
  StringRef dir = sys::path::parent_path(path);
  if (dir.empty())
    dir = ".";
  return sys::fs::is_directory(dir) &&
         !sys::fs::access(dir, sys::fs::AccessMode::Write);
}

void printHelp(raw_ostream &os) {
  Registry &r = Registry::get();
  std::vector<Tag *> tags;
  {
    std::lock_guard<std::mutex> g(r.m);
    tags = r.tags;
  }
  llvm::sort(tags, [](Tag *a, Tag *b) { return a->name() < b->name(); });
  size_t w = 0;
  for (Tag *t : tags)
    w = std::max(w, t->name().size());
  os << "log tags (--log=<pattern>[:<level>]; level 1 flow, 2 object, 3 "
        "detail; docs/reference/logging.md):\n";
  for (Tag *t : tags)
    os << "  " << left_justify(t->name(), w) << "  " << t->maxLevel() << "  "
       << t->description() << "\n";
  os << "  " << left_justify("warn.<kind>", w)
     << "  2  every occurrence of warning <kind>, not only the first\n";
  os.flush();
}

/// One token: bare in the text; else quoted.
bool isBare(StringRef s) {
  if (s.empty())
    return false;
  return llvm::all_of(s, [](char c) {
    return isPrint(c) && c != ' ' && c != '"' && c != '=' && c != '\\';
  });
}

void escapeTo(std::string &out, StringRef s) {
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
        out += "\\x";
        out += hexdigit(static_cast<unsigned char>(c) >> 4, /*LowerCase=*/true);
        out += hexdigit(static_cast<unsigned char>(c) & 15, /*LowerCase=*/true);
      } else {
        out += c;
      }
    }
  }
}

std::string quoteIfNeeded(StringRef s) {
  if (isBare(s))
    return s.str();
  std::string q = "\"";
  escapeTo(q, s);
  q += '"';
  return q;
}

cl::list<std::string>
    logOpt("log",
           cl::desc("Select logs: comma-separated <pattern>[:<level>], "
                    "pattern = a tag, <prefix>.* or *; level 1-3 (default "
                    "1), 0 = off; after SEA_LOG, later entries win. "
                    "--log=help lists the tags. warn.<kind>:2 prints every "
                    "occurrence of a warning"),
           cl::value_desc("pattern[:level],..."), cl::ZeroOrMore,
           cl::cb<void, const std::string &>([](const std::string &v) {
             init();
             if (v == "help") {
               printHelp(outs());
               std::exit(0);
             }
             addSpec(v, "--log");
           }));

cl::opt<std::string> logFileOpt(
    "log-file",
    cl::desc("Append log events to this file instead of stderr (also "
             "SEA_LOG_FILE)"),
    cl::value_desc("path"),
    cl::cb<void, const std::string &>([](const std::string &v) {
      init();
      // an explicit file that cannot be written is an error now, not at the
      // first event (the file is not created here)
      if (!writable(v)) {
        diag::error("log.file", "--log-file: cannot write '" + v + "'");
        std::exit(1);
      }
      setFile(v, "--log-file");
    }));

} // namespace

Tag::Tag(const char *name, unsigned maxLevel, const char *description)
    : nm(name), desc(description), maxLvl(maxLevel) {
  if (maxLevel < 1 || maxLevel > MaxLevel)
    report_fatal_error(Twine("log tag '") + name + "': max level " +
                       Twine(maxLevel) + " is not 1-3");
  Registry &r = Registry::get();
  std::lock_guard<std::mutex> g(r.m);
  for (Tag *t : r.tags)
    if (t->name() == name)
      report_fatal_error(Twine("log tag '") + name + "' declared twice");
  r.tags.push_back(this);
  lvl.store(levelOf(r.sels, name), std::memory_order_relaxed);
}

Tag::~Tag() {
  Registry &r = Registry::get();
  std::lock_guard<std::mutex> g(r.m);
  llvm::erase(r.tags, this);
}

void init() {
  Registry &r = Registry::get();
  {
    std::lock_guard<std::mutex> g(r.m);
    if (r.envRead)
      return;
    r.envRead = true;
  }
  if (const char *f = std::getenv("SEA_LOG_FILE"); f && *f)
    setFile(f, "SEA_LOG_FILE");
  if (const char *s = std::getenv("SEA_LOG"))
    addSpec(s, "SEA_LOG");
}

void select(StringRef spec, StringRef source) {
  init();
  addSpec(spec, source);
}

void setFormat(out::Format f) {
  Registry &r = Registry::get();
  std::lock_guard<std::mutex> g(r.outLock);
  r.format = f;
}

bool warningSelected(StringRef kind) {
  Registry &r = Registry::get();
  std::lock_guard<std::mutex> g(r.m);
  if (r.sels.empty())
    return false;
  std::string w = ("warn." + kind).str();
  for (const Selector &s : llvm::reverse(r.sels)) {
    if (matches(s.pattern, w))
      return s.level >= 2;
    if (s.pattern == kind)
      return s.level >= 1;
  }
  return false;
}

bool parseSpec(StringRef spec, std::vector<Selector> &out,
               std::vector<std::string> &errs) {
  SmallVector<StringRef> items;
  spec.split(items, ',', -1, /*KeepEmpty=*/false);
  bool ok = true;
  for (StringRef item : items) {
    item = item.trim();
    if (item.empty())
      continue;
    auto [pat, lvl] = item.split(':');
    pat = pat.trim();
    lvl = lvl.trim();
    Selector s;
    s.pattern = pat.str();
    if (item.contains(':')) {
      unsigned l;
      if (lvl.getAsInteger(10, l) || l > MaxLevel) {
        errs.push_back(("'" + item + "': the level is 0-3").str());
        ok = false;
        continue;
      }
      s.level = l;
    }
    // a name of [A-Za-z0-9._-], `<prefix>.*` or `*`
    StringRef body = pat;
    if (pat != "*")
      body.consume_back(".*");
    bool okName =
        pat == "*" || (!body.empty() && llvm::all_of(body, [](char c) {
          return isAlnum(c) || c == '.' || c == '_' || c == '-';
        }));
    if (!okName) {
      errs.push_back(
          ("'" + item + "': a pattern is a tag, <prefix>.* or *").str());
      ok = false;
      continue;
    }
    out.push_back(std::move(s));
  }
  return ok;
}

std::string textValue(StringRef v) { return quoteIfNeeded(v); }

bool matches(StringRef pattern, StringRef name) {
  if (pattern == "*")
    return true;
  if (pattern.consume_back(".*"))
    return name == pattern ||
           (name.starts_with(pattern) && name.size() > pattern.size() &&
            name[pattern.size()] == '.');
  return pattern == name;
}

unsigned levelOf(ArrayRef<Selector> sels, StringRef name) {
  for (const Selector &s : llvm::reverse(sels))
    if (matches(s.pattern, name))
      return s.level;
  return 0;
}

// -- Event ------------------------------------------------------------------

Event &Event::msg(const Twine &m) {
  message = m.str();
  return *this;
}

Event &Event::at(mlir::Location l) {
  loc.clear();
  raw_string_ostream os(loc);
  os << l;
  return *this;
}

Event &Event::field(StringRef key, std::string text, std::string json) {
  assert(key != "log" && key != "level" && key != "msg" && key != "loc" &&
         key != "text" && "a reserved key of the jsonl event");
  fields.push_back({key.str(), std::move(text), std::move(json)});
  return *this;
}

Event &Event::num(StringRef key, int64_t v) {
  return field(key, std::to_string(v), out::jnum(v));
}

Event &Event::unum(StringRef key, uint64_t v) {
  return field(key, std::to_string(v), out::jnum(v));
}

Event &Event::real(StringRef key, double v) {
  if (!std::isfinite(v)) {
    const char *s = std::isnan(v) ? "nan" : v > 0 ? "inf" : "-inf";
    return field(key, s, out::jstr(s));
  }
  std::string s;
  raw_string_ostream(s) << format("%.3f", v);
  return field(key, s, s);
}

Event &Event::str(StringRef key, StringRef v) {
  return field(key, quoteIfNeeded(v), out::jstr(v));
}

Event::~Event() {
  std::string text = "log: ";
  text += tag.name();
  if (!message.empty()) {
    text += ' ';
    escapeTo(text, message);
  }
  for (const Field &f : fields) {
    text += ' ';
    text += f.key;
    text += '=';
    text += f.text;
  }
  if (!loc.empty()) {
    text += " at ";
    escapeTo(text, loc);
  }

  Registry &r = Registry::get();
  std::lock_guard<std::mutex> g(r.outLock);
  std::string line;
  if (r.format == out::Format::Jsonl) {
    line =
        "{\"log\":" + out::jstr(tag.name()) + ",\"level\":" + out::jnum(level);
    if (!message.empty())
      line += ",\"msg\":" + out::jstr(message);
    for (const Field &f : fields)
      line += "," + out::jstr(f.key) + ":" + f.json;
    if (!loc.empty())
      line += ",\"loc\":" + out::jstr(loc);
    line += ",\"text\":" + out::jstr(text) + "}";
  } else {
    line = std::move(text);
  }
  line += '\n';
  raw_ostream &os = r.sink();
  os << line;
  os.flush();
}

} // namespace sea::log
