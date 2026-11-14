// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- TimerTest.cpp - tests of the timers (sea/Support/Timer.h) ----------===//
//
// Plain-main driver (no gtest): prints `key: value` lines, exits non-zero if
// a check fails. Run by test/Tools/timer.test. Every check but `system`
// runs on a FakeClock.
//
//  text       secondsText: "%.3f", rounding, 0.
//  stopwatch  seconds since the start, restart().
//  timer      start/stop accumulates wall seconds and counts intervals; a
//             stop without a start is ignored; CPU seconds only when asked.
//  scope      TimeScope counts its lifetime, an early return included; with
//             a parent, the timer's name is a nested --mlir-timing scope.
//  group      timers in first-use order, the same timer per name, keys
//             (with and without a prefix), lap() charges the time since the
//             previous lap (restartLap() skips), CPU laps.
//  system     the real clocks: non-negative, monotone.
//
//===----------------------------------------------------------------------===//

#include "sea/Support/Timer.h"

#include "mlir/Support/Timing.h"
#include "llvm/Support/raw_ostream.h"

#include <cmath>
#include <string>
#include <vector>

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

/// equal up to the rounding of a duration to the clock's tick
bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

void testText() {
  CHECK(sea::secondsText(0) == "0.000", "zero");
  CHECK(sea::secondsText(1.5) == "1.500", "fixed, 3 digits");
  CHECK(sea::secondsText(0.0004) == "0.000", "rounds down");
  CHECK(sea::secondsText(0.0006) == "0.001", "rounds up");
  CHECK(sea::secondsText(1234.5678) == "1234.568", "no exponent");
  llvm::outs() << "text: ok\n";
}

void testStopwatch(sea::FakeClock &clk) {
  sea::Stopwatch w;
  CHECK(near(w.seconds(), 0), "fresh");
  clk.advance(2.25);
  CHECK(near(w.seconds(), 2.25), "elapsed");
  clk.advance(0.75);
  CHECK(near(w.seconds(), 3), "elapsed again");
  w.restart();
  clk.advance(0.5);
  CHECK(near(w.seconds(), 0.5), "restart");
  llvm::outs() << "stopwatch: ok\n";
}

void testTimer(sea::FakeClock &clk) {
  sea::Timer t("exec");
  CHECK(t.name() == "exec", "name");
  CHECK(t.count() == 0 && t.seconds() == 0 && !t.running(), "fresh");
  t.start();
  CHECK(t.running(), "running");
  clk.advance(1.0, 0.5);
  t.stop();
  clk.advance(10.0); // between intervals: not counted
  t.start();
  clk.advance(0.25, 0.25);
  t.stop();
  CHECK(near(t.seconds(), 1.25), "accumulates");
  CHECK(t.count() == 2, "counts intervals");
  CHECK(t.cpuSeconds() == 0 && !t.recordsCpu(), "no CPU unless asked");
  t.stop(); // not running: ignored
  CHECK(t.count() == 2 && near(t.seconds(), 1.25), "stop without start");

  sea::Timer c("solve", /*cpu=*/true);
  c.start();
  clk.advance(2.0, 1.5);
  c.stop();
  CHECK(c.recordsCpu(), "records CPU");
  CHECK(near(c.seconds(), 2.0) && near(c.cpuSeconds(), 1.5), "CPU seconds");

  sea::Timer a("lap");
  a.add(0.5, 0.25);
  CHECK(a.count() == 1 && near(a.seconds(), 0.5), "add");
  llvm::outs() << "timer: ok\n";
}

/// returns early from inside the scope when \p early
int scoped(sea::Timer &t, sea::FakeClock &clk, bool early) {
  sea::TimeScope s(t);
  clk.advance(1.0);
  if (early)
    return 1;
  clk.advance(2.0);
  return 0;
}

void testScope(sea::FakeClock &clk) {
  sea::Timer t("phase");
  scoped(t, clk, /*early=*/false);
  CHECK(near(t.seconds(), 3.0) && t.count() == 1, "scope");
  scoped(t, clk, /*early=*/true);
  CHECK(near(t.seconds(), 4.0) && t.count() == 2, "early exit counts");
  CHECK(!t.running(), "stopped at scope end");

  // nested into --mlir-timing: the phase shows under the root
  std::string report;
  {
    llvm::raw_string_ostream os(report);
    mlir::DefaultTimingManager tm;
    tm.setEnabled(true);
    tm.setOutput(mlir::createOutputStrategy(
        mlir::DefaultTimingManager::OutputFormat::Text, os));
    {
      mlir::TimingScope root = tm.getRootScope();
      sea::Timer n("sea-phase");
      {
        sea::TimeScope s(n, &root);
        clk.advance(1.0);
      }
      CHECK(n.count() == 1, "nested scope counts");
    }
    tm.print();
  }
  CHECK(report.find("sea-phase") != std::string::npos,
        "nested scope in --mlir-timing");
  // a disabled manager: no nesting, the timer still counts
  {
    mlir::DefaultTimingManager tm;
    mlir::TimingScope root = tm.getRootScope();
    sea::Timer n("off");
    { sea::TimeScope s(n, &root); }
    CHECK(n.count() == 1, "disabled parent");
  }
  llvm::outs() << "scope: ok\n";
}

void testGroup(sea::FakeClock &clk) {
  sea::TimerGroup g("vc");
  CHECK(g.empty(), "fresh");
  clk.advance(1.0);
  g.lap("setup");
  clk.advance(2.0);
  g.lap("encode");
  clk.advance(0.5);
  g.lap("setup"); // a repeated name accumulates, keeps its place
  clk.advance(4.0);
  g.restartLap(); // not charged
  clk.advance(0.25);
  g.lap("solve");
  sea::Timer &x = g.timer("exec"); // a timer made by name
  x.start();
  clk.advance(3.0);
  x.stop();
  CHECK(&g.timer("exec") == &x, "same timer per name");
  CHECK(g.find("missing") == nullptr && g.size() == 4, "find");

  std::vector<std::string> keys;
  std::vector<double> secs;
  std::vector<uint64_t> counts;
  g.report([&](llvm::StringRef key, const sea::Timer &t) {
    keys.push_back(key.str());
    secs.push_back(t.seconds());
    counts.push_back(t.count());
  });
  std::vector<std::string> wantKeys{"vc.setup-time", "vc.encode-time",
                                    "vc.solve-time", "vc.exec-time"};
  CHECK(keys == wantKeys, "first-use order and keys");
  CHECK(secs.size() == 4 && near(secs[0], 1.5) && near(secs[1], 2.0) &&
            near(secs[2], 0.25) && near(secs[3], 3.0),
        "lap seconds");
  CHECK(counts == (std::vector<uint64_t>{2, 1, 1, 1}), "lap counts");

  sea::TimerGroup top;
  CHECK(top.key("vc") == "vc-time", "key without prefix");
  sea::TimerGroup seam("seam");
  CHECK(seam.key("rewrite") == "seam.rewrite-time", "key with prefix");

  sea::TimerGroup cpu("p", /*cpu=*/true);
  clk.advance(1.0, 0.75);
  cpu.lap("a");
  CHECK(cpu.find("a") && near(cpu.find("a")->cpuSeconds(), 0.75) &&
            cpu.find("a")->recordsCpu(),
        "CPU lap");
  llvm::outs() << "group: ok\n";
}

void testSystem() {
  sea::Stopwatch w;
  double c0 = sea::cpuSecondsNow();
  volatile double sink = 0;
  for (int i = 0; i < 1000000; ++i)
    sink = sink + i;
  double s = w.seconds();
  CHECK(s >= 0, "wall non-negative");
  CHECK(sea::cpuSecondsNow() >= c0 && c0 >= 0, "CPU monotone");
  llvm::outs() << "system: ok\n";
}

} // namespace

int main() {
  sea::FakeClock clk;
  clk.advance(100.0, 100.0); // away from the epoch
  sea::setFakeClock(&clk);
  testText();
  testStopwatch(clk);
  testTimer(clk);
  testScope(clk);
  testGroup(clk);
  sea::setFakeClock(nullptr);
  testSystem();
  llvm::outs() << "failures: " << failures << "\n";
  return failures ? 1 : 0;
}
