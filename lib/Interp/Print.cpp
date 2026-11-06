// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Print.cpp - rendering of counterexample prints -----------*- C++ -*-===//
//
// The text of sea.print / sea.print.mem output (docs/design/Print.md):
// integers with MLIR type suffixes, pointers as object + offset (`&heap.3[8]`),
// hex dumps in SeaHorn's HexDump layout. The interpreter decides what to
// show; this file only formats it.
//
//===----------------------------------------------------------------------===//

#include "sea/Interp/Interpreter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using llvm::APInt;

namespace sea {

static std::string dec(const APInt &v, bool isSigned) {
  return llvm::toString(v, 10, isSigned);
}
static std::string hex(const APInt &v) {
  return "0x" + llvm::toString(v, 16, /*Signed=*/false, /*formatAsCLiteral=*/
                               false, /*UpperCase=*/false);
}

std::string formatPrintInt(const APInt &v, StringRef fmt) {
  unsigned w = v.getBitWidth();
  std::string iw = std::to_string(w);
  auto boolean = [&]() -> std::string {
    if (v.isZero())
      return "false";
    if (v.isOne())
      return "true";
    return hex(v) + " : i" + iw + " (not a bool)";
  };
  if (fmt == "u")
    return dec(v, false) + " : ui" + iw;
  if (fmt == "s")
    return dec(v, true) + " : si" + iw;
  if (fmt == "x")
    return hex(v) + " : i" + iw;
  if (fmt == "bits")
    return "0b" + llvm::toString(v, 2, false) + " : i" + iw;
  if (fmt == "b")
    return boolean();
  if (fmt == "c") {
    if (v.getActiveBits() <= 8) {
      uint8_t c = v.getZExtValue();
      std::string s = "'";
      if (c == '\'' || c == '\\')
        s += std::string("\\") + char(c);
      else if (llvm::isPrint(c))
        s += char(c);
      else
        s += "\\x" + llvm::utohexstr(c, /*LowerCase=*/true, 2);
      return s + "' : i" + iw;
    }
    return hex(v) + " : i" + iw + " (not a char)";
  }
  // auto (cvlr): an i1 as a bool; unsigned decimal below 2^32; above, hex,
  // plus the signed value when it is negative and small
  if (w == 1)
    return boolean();
  if (v.getActiveBits() <= 32)
    return dec(v, false) + " : ui" + iw;
  std::string s = hex(v) + " : i" + iw;
  if (v.isNegative() && v.getSignificantBits() <= 32)
    s += " (" + dec(v, true) + ")";
  return s;
}

std::string pointerToken(const ObjectInfo &o, const APInt &off) {
  std::string offText = dec(off, /*isSigned=*/true);
  if (o.kind == "null")
    return off.isZero() ? "null" : "&null[" + offText + "]";
  if (o.kind == "func" && off.isZero())
    return "&" + o.name;
  std::string s = "&" + o.name + "[" + offText + "]";
  APInt size = o.size.zextOrTrunc(off.getBitWidth());
  if (!o.alive)
    s += " freed";
  else if (off.isNegative() || off.ugt(size))
    s += " oob";
  else if (off == size && !size.isZero())
    s += " end";
  return s;
}

/// The placed object whose address range [base, base + size) holds `addr`.
static const ObjectInfo *objectAt(ArrayRef<ObjectInfo> objects,
                                  const APInt &addr) {
  for (const ObjectInfo &o : objects) {
    if (!o.base || o.kind == "null")
      continue;
    unsigned w = std::max(addr.getBitWidth(), o.base->getBitWidth()) + 1;
    APInt a = addr.zext(w), b = o.base->zext(w), s = o.size.zextOrTrunc(w);
    if (a.uge(b) && a.ult(b + s))
      return &o;
  }
  return nullptr;
}

std::string addressToken(ArrayRef<ObjectInfo> objects, const APInt &addr) {
  if (addr.isZero())
    return "null";
  std::string s = hex(addr);
  if (const ObjectInfo *o = objectAt(objects, addr))
    s += " ~" + o->name + "[" +
         dec(addr - o->base->zextOrTrunc(addr.getBitWidth()), false) + "]";
  return s;
}

std::string locateAddress(ArrayRef<ObjectInfo> objects, const APInt &addr) {
  const ObjectInfo *o = objectAt(objects, addr);
  if (!o)
    return "";
  return "&" + o->name + "[" +
         dec(addr - o->base->zextOrTrunc(addr.getBitWidth()), false) + "]";
}

out::Fields locateAddressFields(ArrayRef<ObjectInfo> objects,
                                const APInt &addr) {
  const ObjectInfo *o = objectAt(objects, addr);
  if (!o)
    return {};
  return {{"obj", out::jstr(o->name)},
          {"off", out::jint(addr - o->base->zextOrTrunc(addr.getBitWidth()))}};
}

std::string formatObject(const ObjectInfo &o) {
  std::string s;
  llvm::raw_string_ostream os(s);
  os << o.name << " kind=" << o.kind << " size=" << dec(o.size, false);
  if (!o.label.empty())
    os << " label=" << o.label;
  if (o.loc) {
    os << " at=";
    printFileLoc(os, *o.loc);
    printInlinedAt(os, o.inlinedAt);
  }
  if (!o.locName.empty())
    os << " at_name=" << o.locName;
  if (o.base)
    os << " base=" << hex(*o.base);
  if (!o.alive)
    os << " freed";
  return s;
}

out::Fields objectFields(const ObjectInfo &o) {
  out::Fields f{{"id", out::jstr(o.name)},
                {"objkind", out::jstr(o.kind)},
                {"size", out::jint(o.size)}};
  if (!o.label.empty())
    f.push_back({"label", out::jstr(o.label)});
  if (o.loc) {
    f.push_back({"at", out::jstr(fileLocText(*o.loc))});
    if (!o.inlinedAt.empty())
      f.push_back({"at_inlined", jsonCallSites(o.inlinedAt)});
  }
  if (!o.locName.empty())
    f.push_back({"at_name", out::jstr(o.locName)});
  if (o.base)
    f.push_back({"base", out::jint(*o.base)});
  if (!o.alive)
    f.push_back({"freed", "true"});
  return f;
}

out::Fields printIntFields(const APInt &v, StringRef fmt) {
  // auto prints an i1 as a bool (renderInt): the value is a JSON boolean
  bool asBool = v.getBitWidth() == 1 && fmt == "auto";
  return {{"type", out::jstr("i" + std::to_string(v.getBitWidth()))},
          {"fmt", out::jstr(fmt)},
          {"value", asBool ? out::jbool(v.isOne()) : out::jint(v, fmt == "s")}};
}

out::Fields pointerFields(const ObjectInfo &o, const APInt &off) {
  out::Fields f{{"obj", out::jstr(o.kind == "null" ? "null" : o.name)},
                {"off", out::jint(off, /*isSigned=*/true)}};
  APInt size = o.size.zextOrTrunc(off.getBitWidth());
  if (o.kind == "null" || (o.kind == "func" && off.isZero()))
    return f;
  if (!o.alive)
    f.push_back({"state", out::jstr("freed")});
  else if (off.isNegative() || off.ugt(size))
    f.push_back({"state", out::jstr("oob")});
  else if (off == size && !size.isZero())
    f.push_back({"state", out::jstr("end")});
  return f;
}

SmallVector<std::string> hexDumpRows(ArrayRef<DumpByte> bytes) {
  SmallVector<std::string> rows;
  constexpr size_t kRow = 16;
  // offset digits: at least 2, enough for the last offset
  unsigned digits = 2;
  for (size_t last = bytes.empty() ? 0 : bytes.size() - 1; last >= 0x100;
       last >>= 4)
    ++digits;
  std::string prev;
  bool squeezing = false;
  for (size_t at = 0; at < bytes.size(); at += kRow) {
    size_t n = std::min(kRow, bytes.size() - at);
    std::string hexCol, ascii;
    for (size_t i = 0; i < n; ++i) {
      const DumpByte &b = bytes[at + i];
      if (i)
        hexCol += " ";
      if (!b.value) {
        hexCol += "??";
        ascii += "?";
      } else if (b.dflt) {
        hexCol += "..";
        ascii += ".";
      } else {
        hexCol += llvm::utohexstr(*b.value, /*LowerCase=*/true, 2);
        ascii += llvm::isPrint(*b.value) ? char(*b.value) : '.';
      }
    }
    std::string body =
        hexCol + std::string((kRow - n) * 3, ' ') + " |" + ascii + "|";
    bool last = at + kRow >= bytes.size();
    if (body == prev && n == kRow && !last) {
      if (!squeezing)
        rows.push_back("*");
      squeezing = true;
      continue;
    }
    squeezing = false;
    prev = body;
    rows.push_back("+" + llvm::utohexstr(at, /*LowerCase=*/true, digits) +
                   ": " + body);
  }
  return rows;
}

void printPrintRecords(out::Emitter &out, const RunResult &result,
                       PrintMode mode, bool locs) {
  if (mode == PrintMode::Off)
    return;
  for (const PrintRecord &r : result.prints) {
    std::string indent(2 * r.depth, ' ');
    std::string text;
    llvm::raw_string_ostream os(text);
    SourcePositions loc;
    std::optional<std::string> locName;
    if (r.op) {
      loc = positionsOf(r.op->getLoc());
      locName = locNameOf(r.op->getLoc());
    }
    auto at = [&]() {
      if (locs && !loc.empty()) {
        os << " at=";
        printPositions(os, loc);
      }
      if (locs && locName)
        os << " at_name=" << *locName;
    };
    out::Record rec(r.jkind.empty() ? "print" : r.jkind);
    switch (r.kind) {
    case PrintRecord::Scope:
      if (mode != PrintMode::Tree)
        continue;
      rec.kind = "print.scope";
      rec.str("path", r.path).str("tag", r.text);
      os << "print: " << indent << r.text << ":";
      at();
      break;
    case PrintRecord::Line:
      rec.str("path", r.path.empty() ? r.name : r.path + "." + r.name);
      if (!r.path.empty())
        rec.str("scope", r.path);
      rec.add(r.fields);
      os << "print: ";
      if (mode == PrintMode::Tree)
        os << indent << r.text;
      else
        os << (r.path.empty() ? "" : r.path + ".") << r.text;
      at();
      break;
    case PrintRecord::Object:
      rec.kind = "print.object";
      rec.add(r.fields);
      os << "print.object: " << r.text;
      break;
    case PrintRecord::Note:
      rec.kind = "print.note";
      rec.str("message", r.text);
      os << "print.note: " << r.text;
      at();
      break;
    }
    if (mode == PrintMode::Tree && r.kind != PrintRecord::Object)
      rec.num("depth", r.depth);
    if (!loc.empty() && r.kind != PrintRecord::Object) {
      rec.str("at", fileLocText(loc.front()));
      if (loc.size() > 1)
        rec.set("at_inlined", jsonCallSites(ArrayRef(loc).drop_front()));
    }
    if (locName && r.kind != PrintRecord::Object)
      rec.str("at_name", *locName);
    rec.text = os.str();
    out.emit(rec);
  }
  if (result.printsTruncated) {
    out::Record rec("print.truncated");
    rec.num("count", result.printsTruncated);
    rec.text = "print.truncated: " + std::to_string(result.printsTruncated);
    out.emit(rec);
  }
}

} // namespace sea
