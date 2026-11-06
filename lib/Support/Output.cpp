// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Output.cpp - tool output records (text | jsonl) ----------*- C++ -*-===//

#include "sea/Support/Output.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

using namespace llvm;

namespace sea::out {

std::string jstr(StringRef s) {
  std::string out;
  raw_string_ostream os(out);
  json::OStream(os).value(json::Value(json::fixUTF8(s)));
  return out;
}

static constexpr uint64_t kExact = uint64_t(1) << 53;

std::string jnum(int64_t v) {
  if (v < -int64_t(kExact) || v > int64_t(kExact))
    return "\"" + std::to_string(v) + "\"";
  return std::to_string(v);
}
std::string jnum(uint64_t v) {
  if (v > kExact)
    return "\"" + std::to_string(v) + "\"";
  return std::to_string(v);
}

std::string jint(const APInt &v, bool isSigned) {
  std::string s = toString(v, 10, isSigned);
  unsigned bits = isSigned ? v.getSignificantBits() : v.getActiveBits();
  // exact in a double: magnitude below 2^53
  if (bits <= 53)
    return s;
  return "\"" + s + "\"";
}

std::string jbool(bool b) { return b ? "true" : "false"; }

std::string jstrs(ArrayRef<std::string> xs) {
  std::string out = "[";
  for (size_t i = 0; i < xs.size(); ++i)
    out += (i ? "," : "") + jstr(xs[i]);
  return out + "]";
}

std::string jtoken(StringRef tok) {
  StringRef t = tok;
  bool neg = t.consume_front("-");
  if (t.empty() || !isDigit(t.front()))
    return jstr(tok);
  auto [ip, fp] = t.split('.');
  if (!llvm::all_of(ip, isDigit))
    return jstr(tok);
  if (t.contains('.')) {
    if (fp.empty() || !llvm::all_of(fp, isDigit))
      return jstr(tok);
    return tok.str(); // a decimal fraction: a JSON number as written
  }
  // no leading zeros in JSON (but a lone 0)
  if (ip.size() > 1 && ip.front() == '0')
    return jstr(tok);
  APInt v;
  if (ip.getAsInteger(10, v))
    return jstr(tok);
  if (v.getActiveBits() > 53)
    return jstr(tok);
  return (neg ? "-" : "") + ip.str();
}

bool Record::has(StringRef key) const {
  for (const Field &f : fields)
    if (f.first == key)
      return true;
  return false;
}

std::string Record::json() const {
  std::string out = "{\"kind\":" + jstr(kind);
  for (const Field &f : fields)
    out += "," + jstr(f.first) + ":" + f.second;
  out += ",\"text\":" + jstr(text) + "}";
  return out;
}

/// `k=v` with k a field name ([A-Za-z_][A-Za-z0-9_.-]*) and v non-empty.
static bool isKv(StringRef tok, StringRef &k, StringRef &v) {
  size_t eq = tok.find('=');
  if (eq == StringRef::npos || eq == 0 || eq + 1 == tok.size())
    return false;
  k = tok.take_front(eq);
  v = tok.drop_front(eq + 1);
  if (!isAlpha(k.front()) && k.front() != '_')
    return false;
  return llvm::all_of(
      k, [](char c) { return isAlnum(c) || c == '_' || c == '.' || c == '-'; });
}

Record parseLine(StringRef line) {
  if (line.starts_with("BRUNCH_STAT ")) {
    auto [key, value] = line.drop_front(12).split(' ');
    Record r("stat");
    r.str("key", key).set("value", jtoken(value)).set("brunch", "true");
    r.text = line.str();
    return r;
  }
  size_t colon = line.find(':');
  StringRef key =
      colon == StringRef::npos ? StringRef() : line.take_front(colon);
  bool keyed = !key.empty() && !key.contains(' ') &&
               (colon + 1 == line.size() || line[colon + 1] == ' ');
  if (!keyed) {
    Record r("line");
    r.text = line.str();
    return r;
  }
  StringRef rest = line.drop_front(colon + 1).ltrim(' ');
  Record r("stat");
  r.str("key", key);
  SmallVector<StringRef> toks;
  rest.split(toks, ' ', -1, /*KeepEmpty=*/false);
  // the first k=v token; all after it must be k=v too
  size_t first = toks.size();
  StringRef k, v;
  for (size_t i = 0; i < toks.size(); ++i)
    if (isKv(toks[i], k, v)) {
      first = i;
      break;
    }
  bool allKv = first < toks.size();
  for (size_t i = first; allKv && i < toks.size(); ++i)
    allKv = isKv(toks[i], k, v);
  if (!allKv) {
    r.set("value", toks.size() == 1 ? jtoken(rest) : jstr(rest));
  } else {
    if (first > 0) {
      std::string head = join(ArrayRef(toks).take_front(first), " ");
      r.set("value", first == 1 ? jtoken(head) : jstr(head));
    }
    for (size_t i = first; i < toks.size(); ++i) {
      isKv(toks[i], k, v);
      std::string name = k.str();
      while (name == "kind" || name == "text" || r.has(name))
        name += "_";
      r.set(name, jtoken(v));
    }
  }
  r.text = line.str();
  return r;
}

class Emitter::LineStream : public raw_ostream {
public:
  explicit LineStream(Emitter &e) : e(e) { SetUnbuffered(); }
  std::string pending;
  uint64_t pos = 0;

private:
  Emitter &e;
  void write_impl(const char *ptr, size_t size) override {
    pos += size;
    if (!e.jsonl()) {
      e.os.write(ptr, size);
      return;
    }
    StringRef s(ptr, size);
    while (!s.empty()) {
      auto [head, tail] = s.split('\n');
      if (head.size() == s.size()) { // no newline: keep the partial line
        pending += head.str();
        break;
      }
      pending += head.str();
      e.line(pending);
      pending.clear();
      s = tail;
    }
  }
  uint64_t current_pos() const override { return pos; }
};

Emitter::Emitter(raw_ostream &os, Format format)
    : os(os), fmt(format), ls(std::make_unique<LineStream>(*this)) {}

Emitter::~Emitter() { flush(); }

void Emitter::line(StringRef text) { os << parseLine(text).json() << "\n"; }

void Emitter::emit(const Record &r) {
  if (jsonl() && !ls->pending.empty()) {
    std::string p = std::move(ls->pending);
    ls->pending.clear();
    line(p);
  }
  if (jsonl())
    os << r.json() << "\n";
  else
    os << r.text << "\n";
}

raw_ostream &Emitter::lines() { return *ls; }

void Emitter::flush() {
  if (jsonl() && !ls->pending.empty()) {
    std::string p = std::move(ls->pending);
    ls->pending.clear();
    line(p);
  }
  os.flush();
}

} // namespace sea::out
