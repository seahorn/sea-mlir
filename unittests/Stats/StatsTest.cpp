// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- StatsTest.cpp - tests of the stat lines (sea/Support/Stats.h) ------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if
// a check fails. Run by test/Tools/stats.test.
//
//  forms   a table of sea-vc's stat line shapes, built with the Line
//          builders, and their exact text and BRUNCH_STAT lines under the
//          Derived rule. The expected strings are sea-vc output from before
//          the port (one token, several tokens, an empty value, assume.*
//          with a state word, seam.inb with notes, xrt.pairs with a
//          parenthesised k=v note).
//  rules   Off / Value / Parts / Derived on the same lines; brunchOnly; a
//          per-line policy over the report's.
//  order   print(): every text line, then every BRUNCH_STAT line; none
//          without the flag. printEach(): each line's text line, then its
//          BRUNCH_STAT lines. append(): the lines of another report, with
//          their policies.
//  timers  time(Timer) and timers(TimerGroup) keys and "%.3f" values.
//  mem     memFields: names, order, MiB rounding, unknown fields left out.
//  assume  the assumption ledger (sea/Support/Assume.h): the assume.*
//          lines of sea-vc's SeaM block, of a replay and of the run
//          reports (text pinned to the output before the port), their
//          BRUNCH_STAT keys (incl. assume.addr0-is-null-null-uses), none
//          under Off, sums and runs, add(), runFields.
//
//===----------------------------------------------------------------------===//

#include "sea/Support/Stats.h"
#include "sea/Support/Assume.h"

#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <string>

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

using sea::stats::Brunch;
using sea::stats::Line;
using sea::stats::Report;

/// The text and BRUNCH_STAT lines of one line built by \p build.
struct Rendered {
  std::string text, brunch;
};
Rendered render(Brunch policy, const std::function<void(Report &)> &build) {
  Report r(policy);
  build(r);
  Rendered out;
  out.text = r.text();
  std::string all;
  llvm::raw_string_ostream os(all);
  r.print(os, true);
  out.brunch = all.substr(out.text.size());
  return out;
}

void expect(const char *what, const std::function<void(Report &)> &build,
            const std::string &text, const std::string &brunch,
            Brunch policy = Brunch::Derived) {
  Rendered got = render(policy, build);
  CHECK(got.text == text, what << ": text '" << got.text << "'");
  CHECK(got.brunch == brunch, what << ": brunch '" << got.brunch << "'");
}

void testForms() {
  // one token
  expect(
      "one token", [](Report &r) { r.line("xrt.mode").value("plain"); },
      "xrt.mode: plain\n", "BRUNCH_STAT xrt.mode plain\n");
  expect(
      "number", [](Report &r) { r.line("xrt.reads").value(uint64_t(0)); },
      "xrt.reads: 0\n", "BRUNCH_STAT xrt.reads 0\n");
  expect(
      "seconds", [](Report &r) { r.line("vc.xrt-lower-time").seconds(0.0004); },
      "vc.xrt-lower-time: 0.000\n", "BRUNCH_STAT vc.xrt-lower-time 0.000\n");
  // several tokens: text only
  expect(
      "value and kv",
      [](Report &r) { r.line("xrt.read-leaves").value(0).kv("max", 0); },
      "xrt.read-leaves: 0 max=0\n", "");
  expect(
      "kv only",
      [](Report &r) {
        r.line("xrt.mux").kv("objects", 0).kv("cells", 0).kv("records", 0);
      },
      "xrt.mux: objects=0 cells=0 records=0\n", "");
  expect(
      "two values", [](Report &r) { r.line("xrt.bucket").value(3).value(7); },
      "xrt.bucket: 3 7\n", "");
  // an empty value: `key:`, no BRUNCH_STAT line
  expect(
      "empty", [](Report &r) { r.line("xrt.eq-widths"); }, "xrt.eq-widths:\n",
      "");
  // the same key with one token
  expect(
      "eq-widths one", [](Report &r) { r.line("xrt.eq-widths").value("8x3"); },
      "xrt.eq-widths: 8x3\n", "BRUNCH_STAT xrt.eq-widths 8x3\n");
  // assume.*: a state word, then k=v
  expect(
      "assume state kv",
      [](Report &r) {
        r.line("assume.addr0-is-null").value("vc").kv("null-uses", 0);
      },
      "assume.addr0-is-null: vc null-uses=0\n",
      "BRUNCH_STAT assume.addr0-is-null vc\n"
      "BRUNCH_STAT assume.addr0-is-null-null-uses 0\n");
  expect(
      "assume kv",
      [](Report &r) { r.line("assume.a-inb").kv("drop-fresh-memhavoc", 1); },
      "assume.a-inb: drop-fresh-memhavoc=1\n",
      "BRUNCH_STAT assume.a-inb-drop-fresh-memhavoc 1\n");
  // seam.inb
  expect(
      "seam.inb assumed",
      [](Report &r) {
        r.line("seam.inb")
            .value("assumed")
            .kv("accesses", 0)
            .kv("conditions", 0);
      },
      "seam.inb: assumed accesses=0 conditions=0\n",
      "BRUNCH_STAT seam.inb assumed\n"
      "BRUNCH_STAT seam.inb-accesses 0\n"
      "BRUNCH_STAT seam.inb-conditions 0\n");
  expect(
      "seam.inb off lowering",
      [](Report &r) {
        r.line("seam.inb")
            .value("off")
            .text("")
            .note("lowering: drop-fresh-memhavoc=1 rests on A-INB");
      },
      "seam.inb: off (lowering: drop-fresh-memhavoc=1 rests on A-INB)\n",
      "BRUNCH_STAT seam.inb off\n");
  expect(
      "seam.inb off fallback",
      [](Report &r) {
        r.line("seam.inb")
            .value("off")
            .text(" (fallback: an access leaves its object)")
            .note("lowering: drop-fresh-memhavoc=1 rests on A-INB");
      },
      "seam.inb: off (fallback: an access leaves its object) (lowering: "
      "drop-fresh-memhavoc=1 rests on A-INB)\n",
      "BRUNCH_STAT seam.inb off\n");
  // a parenthesised k=v note: several tokens, text only
  expect(
      "xrt.pairs",
      [](Report &r) {
        r.line("xrt.pairs")
            .value(0)
            .kv("exact-drop", 0)
            .kv("guard-drop", 0)
            .kv("disjoint-drop", 0)
            .kv("solver-array", 0)
            .kv("mux", 0)
            .kv("constraints", 0)
            .note("offset=0 address=0");
      },
      "xrt.pairs: 0 exact-drop=0 guard-drop=0 disjoint-drop=0 "
      "solver-array=0 mux=0 constraints=0 (offset=0 address=0)\n",
      "");
  llvm::outs() << "forms: ok\n";
}

void testRules() {
  auto seamOff = [](Report &r) {
    r.line("seam.inb").value("off").kv("k", 2).note("n=3 x");
  };
  expect("off", seamOff, "seam.inb: off k=2 (n=3 x)\n", "", Brunch::Off);
  expect("value", seamOff, "seam.inb: off k=2 (n=3 x)\n",
         "BRUNCH_STAT seam.inb off k=2 (n=3 x)\n", Brunch::Value);
  expect("parts", seamOff, "seam.inb: off k=2 (n=3 x)\n",
         "BRUNCH_STAT seam.inb off\nBRUNCH_STAT seam.inb-k 2\n", Brunch::Parts);
  // Parts on a key Derived would not split; Derived leaves it out
  auto other = [](Report &r) { r.line("x.y").value("on").kv("n", 1); };
  expect("parts other", other, "x.y: on n=1\n",
         "BRUNCH_STAT x.y on\nBRUNCH_STAT x.y-n 1\n", Brunch::Parts);
  expect("derived other", other, "x.y: on n=1\n", "", Brunch::Derived);
  // a later bare word is not a state
  expect(
      "parts later word",
      [](Report &r) { r.line("assume.z").kv("a", 1).value("w"); },
      "assume.z: a=1 w\n", "BRUNCH_STAT assume.z-a 1\n");
  // brunchOnly: no text line
  expect(
      "brunch only",
      [](Report &r) {
        r.line("vc.ptr-mode")
            .value("address")
            .brunch(Brunch::Value)
            .brunchOnly();
      },
      "", "BRUNCH_STAT vc.ptr-mode address\n", Brunch::Off);
  // a per-line policy over the report's
  expect(
      "per line",
      [](Report &r) {
        r.line("a").value(1);
        r.line("b").value(2).brunch(Brunch::Off);
      },
      "a: 1\nb: 2\n", "BRUNCH_STAT a 1\n");
  llvm::outs() << "rules: ok\n";
}

void testOrder() {
  Report r(Brunch::Derived);
  r.line("a").value(1);
  r.line("b").value("x y");
  r.line("c").value(3);
  std::string with, without;
  llvm::raw_string_ostream w(with), wo(without);
  r.print(w, true);
  r.print(wo, false);
  CHECK(with == "a: 1\nb: x y\nc: 3\nBRUNCH_STAT a 1\nBRUNCH_STAT c 3\n",
        "order: '" << with << "'");
  CHECK(without == "a: 1\nb: x y\nc: 3\n", "no brunch: '" << without << "'");
  CHECK(r.lines().size() == 3 && r.lines()[1].key() == "b", "lines");
  // printEach: interleaved; a brunchOnly line between text lines
  Report e(Brunch::Off);
  e.line("result").value("unknown").brunch(Brunch::Value).brunchOnly();
  e.line("mem-limit").value("hit").brunch(Brunch::Value);
  e.line("mem-limit-phase").value("solve");
  std::string each, eachOff;
  llvm::raw_string_ostream ew(each), ewo(eachOff);
  e.printEach(ew, true);
  e.printEach(ewo, false);
  CHECK(each == "BRUNCH_STAT result unknown\nmem-limit: hit\n"
                "BRUNCH_STAT mem-limit hit\nmem-limit-phase: solve\n",
        "each: '" << each << "'");
  CHECK(eachOff == "mem-limit: hit\nmem-limit-phase: solve\n",
        "each, no brunch: '" << eachOff << "'");
  // append: copies, policies kept, after the lines already there
  Report a(Brunch::Value);
  a.line("x").value(0);
  a.append(e);
  std::string app;
  llvm::raw_string_ostream aw(app);
  a.print(aw, true);
  CHECK(app == "x: 0\nmem-limit: hit\nmem-limit-phase: solve\n"
               "BRUNCH_STAT x 0\nBRUNCH_STAT result unknown\n"
               "BRUNCH_STAT mem-limit hit\n",
        "append: '" << app << "'");
  llvm::outs() << "order: ok\n";
}

void testTimers() {
  sea::FakeClock clk;
  sea::setFakeClock(&clk);
  sea::Timer t("lambda.simplify");
  t.start();
  clk.advance(1.25);
  t.stop();
  sea::TimerGroup g("vc");
  g.lap("encode");
  clk.advance(0.5);
  g.lap("lower");
  sea::setFakeClock(nullptr);
  expect(
      "timers",
      [&](Report &r) {
        r.time(t);
        r.timers(g);
      },
      "lambda.simplify-time: 1.250\nvc.encode-time: 0.000\n"
      "vc.lower-time: 0.500\n",
      "BRUNCH_STAT lambda.simplify-time 1.250\n"
      "BRUNCH_STAT vc.encode-time 0.000\nBRUNCH_STAT vc.lower-time 0.500\n");
  llvm::outs() << "timers: ok\n";
}

std::string memText(const sea::ProcMem &m) {
  std::string s;
  llvm::raw_string_ostream os(s);
  for (const auto &[k, v] : sea::memFields(m))
    os << " " << k << "=" << v;
  return s;
}

void testMem() {
  sea::ProcMem m;
  CHECK(memText(m).empty(), "nothing known");
  m.peakRssKB = 1536;
  m.hasPeak = true;
  CHECK(memText(m) == " peak-rss-mb=2", "peak only, rounded: " << memText(m));
  m.rssKB = 1535;
  m.vmKB = 10240;
  m.hasCurrent = true;
  CHECK(memText(m) == " rss-mb=1 vm-mb=10 peak-rss-mb=2",
        "all: " << memText(m));
  m.hasPeak = false;
  CHECK(memText(m) == " rss-mb=1 vm-mb=10", "current only: " << memText(m));
  llvm::outs() << "mem: ok\n";
}

} // namespace

void testAssume() {
  using sea::assume::Ledger;
  using sea::assume::Name;
  // sea-vc's SeaM block: every VC line, in their order
  Ledger vc;
  vc.site(Name::MemhavocNullNoop, "sites", 2);
  vc.site(Name::Addr0IsNull, "null-uses", 0);
  vc.site(Name::AInb, "drop-fresh-memhavoc", 3);
  vc.site(Name::MemsetBound, "count", 1);
  vc.site(Name::MemsetBound, "bound", 64);
  Rendered r = render(Brunch::Derived, [&](Report &rep) { vc.report(rep); });
  CHECK(r.text == "assume.memset-bound: count=1 bound=64\n"
                  "assume.a-inb: drop-fresh-memhavoc=3\n"
                  "assume.addr0-is-null: vc null-uses=0\n"
                  "assume.memhavoc-null-noop: vc sites=2\n",
        r.text);
  CHECK(r.brunch == "BRUNCH_STAT assume.memset-bound-count 1\n"
                    "BRUNCH_STAT assume.memset-bound-bound 64\n"
                    "BRUNCH_STAT assume.a-inb-drop-fresh-memhavoc 3\n"
                    "BRUNCH_STAT assume.addr0-is-null vc\n"
                    "BRUNCH_STAT assume.addr0-is-null-null-uses 0\n"
                    "BRUNCH_STAT assume.memhavoc-null-noop vc\n"
                    "BRUNCH_STAT assume.memhavoc-null-noop-sites 2\n",
        r.brunch);
  // a replay's uses: only the used ones; Parts keys; nothing under Off
  struct Run {
    uint64_t addr0IsNullUses, memhavocNullNoopUses;
  };
  Ledger used;
  used.useRun(Run{5, 0});
  Rendered u = render(Brunch::Parts, [&](Report &rep) { used.report(rep); });
  CHECK(u.text == "assume.addr0-is-null: uses=5\n", u.text);
  CHECK(u.brunch == "BRUNCH_STAT assume.addr0-is-null-uses 5\n", u.brunch);
  Rendered off = render(Brunch::Off, [&](Report &rep) { used.report(rep); });
  CHECK(off.text == u.text && off.brunch.empty(), off.brunch);
  CHECK(Ledger().runFields(false) == "addr0-is-null=0 memhavoc-null-noop=0",
        "runFields of nothing");
  CHECK(!Ledger().anyUse() && used.anyUse(), "anyUse");
  // sums: uses add, a run with uses counts once per name
  Ledger all;
  all.useRun(Run{2, 1});
  all.useRun(Run{0, 4});
  all.add(used);
  CHECK(all.uses(Name::Addr0IsNull) == 7 && all.runs(Name::Addr0IsNull) == 2,
        "addr0 sums");
  CHECK(all.uses(Name::MemhavocNullNoop) == 5 &&
            all.runs(Name::MemhavocNullNoop) == 2,
        "memhavoc sums");
  CHECK(all.runFields(true) == "addr0-is-null=7 addr0-is-null-runs=2 "
                               "memhavoc-null-noop=5 memhavoc-null-noop-runs=2",
        all.runFields(true));
  Rendered a = render(Brunch::Off, [&](Report &rep) { all.report(rep); });
  CHECK(a.text == "assume.addr0-is-null: uses=7\n"
                  "assume.memhavoc-null-noop: uses=5\n",
        a.text);
  llvm::outs() << "assume: ok\n";
}

int main() {
  testForms();
  testRules();
  testOrder();
  testTimers();
  testMem();
  testAssume();
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
