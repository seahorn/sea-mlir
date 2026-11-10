// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- LogTest.cpp - tests of the log selection (sea/Support/Log.h) -------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if
// a check fails. Run by test/Tools/log-spec.test.
//
//  parse    parseSpec: entries, default level 1, :0, empty entries, malformed
//           levels and patterns (the entries before them are kept).
//  match    matches: exact, `a.*` (a itself and a.<rest>, not ab), `*`.
//  level    levelOf: the last matching entry wins; no match is 0.
//  text     textValue: bare tokens, quoting, escapes of control bytes.
//  tag      a registered tag follows --log entries given to the parser of
//           the command line; on(L) is level >= L; events (text and jsonl,
//           non-finite numbers) go to --log-file; an event that is off
//           does not evaluate its arguments.
//
//===----------------------------------------------------------------------===//

#include "sea/Support/Log.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <cmath>
#include <string>
#include <vector>

using namespace sea::log;

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

std::string show(const std::vector<Selector> &sels) {
  std::string s;
  for (const Selector &x : sels)
    s += (s.empty() ? "" : ",") + x.pattern + ":" + std::to_string(x.level);
  return s;
}

/// parseSpec, rendered: the entries, then ` ! <error>` per malformed one
std::string parse(llvm::StringRef spec) {
  std::vector<Selector> sels;
  std::vector<std::string> errs;
  bool ok = parseSpec(spec, sels, errs);
  std::string s = show(sels);
  for (const std::string &e : errs)
    s += " ! " + e;
  if (ok != errs.empty())
    s += " (return value disagrees)";
  return s;
}

void testParse() {
  CHECK(parse("a") == "a:1", parse("a"));
  CHECK(parse("a.b:2,c.*:3,*:0") == "a.b:2,c.*:3,*:0",
        parse("a.b:2,c.*:3,*:0"));
  CHECK(parse("") == "", parse(""));
  CHECK(parse(",a,,b:2,") == "a:1,b:2", parse(",a,,b:2,"));
  CHECK(parse(" a : 2 , b ") == "a:2,b:1", parse(" a : 2 , b "));
  CHECK(parse("a:4") == " ! 'a:4': the level is 0-3", parse("a:4"));
  CHECK(parse("a:x") == " ! 'a:x': the level is 0-3", parse("a:x"));
  CHECK(parse("a:") == " ! 'a:': the level is 0-3", parse("a:"));
  // a malformed entry is skipped; the entries after it still count
  CHECK(parse("a,b:9,c") == "a:1,c:1 ! 'b:9': the level is 0-3",
        parse("a,b:9,c"));
  CHECK(parse("x*,b:2,y:5") == "b:2 ! 'x*': a pattern is a tag, <prefix>.* or "
                               "* ! 'y:5': the level is 0-3",
        parse("x*,b:2,y:5"));
  CHECK(parse("a*") == " ! 'a*': a pattern is a tag, <prefix>.* or *",
        parse("a*"));
  CHECK(parse("*.b").find("!") != std::string::npos, parse("*.b"));
  CHECK(parse(".*").find("!") != std::string::npos, parse(".*"));
  CHECK(parse(":2").find("!") != std::string::npos, parse(":2"));
  llvm::outs() << "parse: ok\n";
}

void testMatch() {
  CHECK(matches("a.b", "a.b"), "exact");
  CHECK(!matches("a.b", "a.bc"), "exact, not a prefix");
  CHECK(!matches("a.b", "a"), "exact, not shorter");
  CHECK(matches("a.*", "a"), "prefix matches its root");
  CHECK(matches("a.*", "a.b"), "prefix");
  CHECK(matches("a.*", "a.b.c"), "prefix, deeper");
  CHECK(!matches("a.*", "ab"), "prefix stops at a dot");
  CHECK(!matches("a.*", "b.a"), "prefix is a prefix");
  CHECK(matches("*", "anything.at.all"), "star");
  llvm::outs() << "match: ok\n";
}

void testLevel() {
  std::vector<Selector> sels;
  std::vector<std::string> err;
  CHECK(parseSpec("*:1,seam.*:2,seam.load:3,seam.x:0", sels, err), "parse");
  CHECK(levelOf(sels, "vc.phase") == 1, "star");
  CHECK(levelOf(sels, "seam.rule") == 2, "prefix after star");
  CHECK(levelOf(sels, "seam.load") == 3, "exact after prefix");
  CHECK(levelOf(sels, "seam.x") == 0, ":0 turns off");
  CHECK(levelOf({}, "a") == 0, "nothing selected");
  sels.clear();
  CHECK(parseSpec("a:3,*:0", sels, err), "parse");
  CHECK(levelOf(sels, "a") == 0, "a later star wins");
  llvm::outs() << "level: ok\n";
}

void testText() {
  CHECK(textValue("abc.d-1/x") == "abc.d-1/x", textValue("abc.d-1/x"));
  CHECK(textValue("") == "\"\"", textValue(""));
  CHECK(textValue("a b") == "\"a b\"", textValue("a b"));
  CHECK(textValue("k=v") == "\"k=v\"", textValue("k=v"));
  CHECK(textValue("q\"\\") == "\"q\\\"\\\\\"", textValue("q\"\\"));
  CHECK(textValue("a\nb\rc\td") == "\"a\\nb\\rc\\td\"",
        textValue("a\nb\rc\td"));
  CHECK(textValue(llvm::StringRef("\x01\x1f\x7f\0", 4)) ==
            "\"\\x01\\x1f\\x7f\\x00\"",
        textValue(llvm::StringRef("\x01\x1f\x7f\0", 4)));
  llvm::outs() << "text: ok\n";
}

Tag tagA("test.a", 3, "a test tag");
Tag tagB("test.b", 1, "another test tag");

void testTag(int argc, char **argv) {
  CHECK(!tagA.on(1) && !tagB.on(1), "off before --log");
  llvm::cl::ParseCommandLineOptions(argc, argv, "sea-log-test\n");
  // the lit test passes --log=test.*:2 --log=test.b:0
  CHECK(tagA.level() == 2, "prefix sets level 2");
  CHECK(tagA.on(1) && tagA.on(2) && !tagA.on(3), "on(L) is level >= L");
  CHECK(!tagB.on(1), "a later entry turns test.b off");
  // a tag registered after the selection gets it
  Tag tagC("test.c", 2, "registered late");
  CHECK(tagC.level() == 2, "late registration");
  // events: to --log-file (the lit test checks the file); one text, one jsonl
  SEA_LOG(tagA, 2)
      .msg("m\r1")
      .kv("s", "a\rb\x01")
      .kv("n", -3)
      .kv("nan", std::nan(""))
      .kv("inf", -HUGE_VAL)
      .kv("x", 0.5);
  setFormat(sea::out::Format::Jsonl);
  SEA_LOG(tagA, 2).kv("nan", std::nan("")).kv("inf", HUGE_VAL);
  setFormat(sea::out::Format::Text);
  bool evaluated = false;
  SEA_LOG(tagA, 3).kv("never", (evaluated = true));
  CHECK(!evaluated, "arguments of an event that is off are not evaluated");
  llvm::outs() << "tag: ok\n";
}

} // namespace

/// --emit: a tool's run, for the lit tests of the selection: SEA_LOG and
/// SEA_LOG_FILE (init), then the options; one event per level of each tag.
int emit(int argc, char **argv) {
  static llvm::cl::opt<bool> emitOpt("emit", llvm::cl::desc("emit events"));
  static llvm::cl::opt<bool> jsonl("jsonl", llvm::cl::desc("jsonl events"));
  init();
  llvm::cl::ParseCommandLineOptions(argc, argv, "sea-log-test --emit\n");
  if (jsonl)
    setFormat(sea::out::Format::Jsonl);
  for (Tag *t : {&tagA, &tagB})
    for (unsigned l = 1; l <= t->maxLevel(); ++l)
      SEA_LOG(*t, l).msg("event").kv("l", l);
  llvm::outs() << "done\n";
  return 0;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i)
    if (llvm::StringRef(argv[i]) == "--emit")
      return emit(argc, argv);
  testParse();
  testMatch();
  testLevel();
  testText();
  testTag(argc, argv);
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
