// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Output.h - tool output records (text | jsonl) ------------*- C++ -*-===//
//
// Every line a tool (sea-vc, sea-run) prints on stdout goes through an
// Emitter as a Record: a kind, structured fields, and its text rendering.
// The text format prints the rendering (`key: value` lines, unchanged); the
// jsonl format prints one JSON object per line:
//
//   {"kind":"<kind>", <fields>..., "text":"<the text line(s)>"}
//
// so the two formats cannot drift: `jq -r .text` of the jsonl output is the
// text output.
//
// Records come two ways:
//  - built by the caller (Record + emit): counterexample lines with
//    structured values (nondet, replay, print, ...);
//  - text written to lines(): each complete line is parsed (parseLine)
//    into a `stat` record: `key: value [k=v ...]` -> key, value, fields.
//    In the text format lines() writes through unchanged.
//
// Field values are JSON text: numbers stay numbers when exact (|n| < 2^53),
// larger integers are decimal strings.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_SUPPORT_OUTPUT_H
#define SEA_SUPPORT_OUTPUT_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace sea::out {

enum class Format { Text, Jsonl };

/// A field: its name and its value as JSON text.
using Field = std::pair<std::string, std::string>;
using Fields = std::vector<Field>;

/// JSON text of values.
std::string jstr(llvm::StringRef s);
std::string jnum(int64_t v);
std::string jnum(uint64_t v);
inline std::string jnum(unsigned v) { return jnum(uint64_t(v)); }
inline std::string jnum(int v) { return jnum(int64_t(v)); }
/// An integer: a number when |v| < 2^53, else a decimal string.
std::string jint(const llvm::APInt &v, bool isSigned = false);
std::string jbool(bool b);
/// A JSON array of strings.
std::string jstrs(llvm::ArrayRef<std::string> xs);
/// A number token as it appears in text (`12`, `-3`, `0.125`): a JSON
/// number when it is one (integers exact: < 2^53), else a string.
std::string jtoken(llvm::StringRef tok);

struct Record {
  std::string kind;
  Fields fields;
  /// The text rendering: the line(s) the text format prints, without the
  /// final newline. Several lines only for a record that groups them
  /// (`replay`, `status`).
  std::string text;

  explicit Record(llvm::StringRef kind) : kind(kind.str()) {}
  Record &set(llvm::StringRef key, std::string json) {
    fields.emplace_back(key.str(), std::move(json));
    return *this;
  }
  Record &str(llvm::StringRef key, llvm::StringRef v) {
    return set(key, jstr(v));
  }
  Record &num(llvm::StringRef key, uint64_t v) { return set(key, jnum(v)); }
  Record &snum(llvm::StringRef key, int64_t v) { return set(key, jnum(v)); }
  Record &integer(llvm::StringRef key, const llvm::APInt &v,
                  bool isSigned = false) {
    return set(key, jint(v, isSigned));
  }
  Record &add(const Fields &fs) {
    fields.insert(fields.end(), fs.begin(), fs.end());
    return *this;
  }
  bool has(llvm::StringRef key) const;
  /// The jsonl line (no newline).
  std::string json() const;
};

/// A text line as a record: `BRUNCH_STAT <key> <value>` -> stat with
/// brunch=true; `<key>: <rest>` -> stat {key, value, k=v fields}: <rest> is
/// `<value>`, `<value> k=v ...` or `k=v ...` (every token after the first
/// k=v must be k=v, else all of <rest> is the value); anything else ->
/// {"kind":"line"}.
Record parseLine(llvm::StringRef line);

class Emitter {
public:
  Emitter(llvm::raw_ostream &os, Format format);
  ~Emitter();
  Format format() const { return fmt; }
  bool jsonl() const { return fmt == Format::Jsonl; }
  /// Prints a record: its text (text) or its JSON line (jsonl).
  void emit(const Record &r);
  /// A stream of text lines: written through (text), or each line parsed
  /// into a record (jsonl). Unbuffered: a line is emitted as it completes.
  llvm::raw_ostream &lines();
  /// Emits a pending partial line of lines() and flushes the output.
  void flush();

private:
  class LineStream;
  llvm::raw_ostream &os;
  Format fmt;
  std::unique_ptr<LineStream> ls;
  friend class LineStream;
  void line(llvm::StringRef text);
};

} // namespace sea::out

#endif // SEA_SUPPORT_OUTPUT_H
