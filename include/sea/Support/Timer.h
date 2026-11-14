// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Timer.h - wall (and CPU) time of named phases ------------*- C++ -*-===//
//
// The one way sea-mlir measures time for its `*-time` keys and `time=`
// fields:
//
//  secondsText  the printed form of a duration: "%.3f" seconds.
//  Stopwatch    the wall time since its start (budgets, deadlines, the time
//               of one item); one clock read per start and per reading.
//  Timer        a named accumulator: total wall seconds over its start/stop
//               intervals and their count; with `cpu` also the process CPU
//               seconds of those intervals (one more clock read per start
//               and stop, so off by default).
//  TimeScope    RAII start/stop of a Timer (an early return still counts);
//               optionally a nested mlir::TimingScope, so the phase also
//               shows in --mlir-timing.
//  TimerGroup   the timers of one run (no global registry: a retry or a
//               split part has its own), in first-use order; key() derives
//               the printed key `<prefix>.<name>-time`; lap() charges the
//               time since the previous lap to a named timer.
//
// Wall time is std::chrono::steady_clock; CPU time is the process's CPU
// clock (all threads). Tests replace both with a FakeClock (setFakeClock).
//
// Thread safety: a Timer, TimeScope or TimerGroup belongs to one thread at a
// time; distinct objects may be used on distinct threads.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_SUPPORT_TIMER_H
#define SEA_SUPPORT_TIMER_H

#include "mlir/Support/Timing.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace sea {

using SteadyClock = std::chrono::steady_clock;

/// A clock a test sets by hand (setFakeClock): the readings of every
/// Stopwatch, Timer and TimerGroup while it is installed.
struct FakeClock {
  SteadyClock::time_point wall{};
  double cpu = 0;
  /// Moves the wall clock by \p wallSeconds and the CPU clock by
  /// \p cpuSeconds.
  void advance(double wallSeconds, double cpuSeconds = 0);
};

/// Installs \p clock as the source of all readings below; null restores the
/// system clocks. For tests: install before the timers under test start.
void setFakeClock(FakeClock *clock);

namespace detail {
extern std::atomic<FakeClock *> fakeClock;
} // namespace detail

/// The wall clock now (steady_clock, or the fake clock).
inline SteadyClock::time_point wallNow() {
  if (FakeClock *f = detail::fakeClock.load(std::memory_order_relaxed))
    return f->wall;
  return SteadyClock::now();
}

/// The CPU seconds this process has used so far (all threads), or the fake
/// clock's.
double cpuSecondsNow();

/// Seconds from \p from to \p to.
inline double secondsBetween(SteadyClock::time_point from,
                             SteadyClock::time_point to) {
  return std::chrono::duration<double>(to - from).count();
}

/// The printed form of a duration of \p seconds: "%.3f" (every `*-time` key
/// and `time=` field).
std::string secondsText(double seconds);

/// Wall time since the start (construction or restart()).
class Stopwatch {
public:
  Stopwatch() : t0(wallNow()) {}
  void restart() { t0 = wallNow(); }
  double seconds() const { return secondsBetween(t0, wallNow()); }
  /// The start time point (construction or the last restart()).
  SteadyClock::time_point startTime() const { return t0; }

private:
  SteadyClock::time_point t0;
};

/// A named accumulator of time intervals (see the file comment).
class Timer {
public:
  explicit Timer(std::string name = "", bool cpu = false)
      : timerName(std::move(name)), withCpu(cpu) {}

  /// Starts an interval; the timer must not be running (asserted).
  void start() {
    assert(!isRunning && "sea::Timer started twice");
    t0 = wallNow();
    if (withCpu)
      cpu0 = cpuSecondsNow();
    isRunning = true;
  }
  /// Ends the interval started last: adds its time, counts it. A timer
  /// that is not running is left alone.
  void stop() {
    if (!isRunning)
      return;
    isRunning = false;
    add(secondsBetween(t0, wallNow()), withCpu ? cpuSecondsNow() - cpu0 : 0);
  }
  /// Adds one interval measured elsewhere (TimerGroup::lap). Must not be
  /// called while the timer is running (it would end that interval
  /// uncounted).
  void add(double seconds, double cpuSeconds = 0) {
    assert(!isRunning && "sea::Timer::add while running");
    wall += seconds;
    cpuTotal += cpuSeconds;
    ++n;
  }

  llvm::StringRef name() const { return timerName; }
  bool running() const { return isRunning; }
  bool recordsCpu() const { return withCpu; }
  /// Total wall seconds of the finished intervals.
  double seconds() const { return wall; }
  /// Total CPU seconds of the finished intervals (0 unless recordsCpu()).
  double cpuSeconds() const { return cpuTotal; }
  /// The finished intervals.
  uint64_t count() const { return n; }

private:
  std::string timerName;
  bool withCpu;
  bool isRunning = false;
  SteadyClock::time_point t0{};
  double cpu0 = 0;
  double wall = 0, cpuTotal = 0;
  uint64_t n = 0;
};

/// Times its own lifetime into a Timer; with \p parent, also as a nested
/// --mlir-timing scope named after the timer.
class TimeScope {
public:
  explicit TimeScope(Timer &timer, mlir::TimingScope *parent = nullptr)
      : timer(timer) {
    if (parent)
      nested = parent->nest(timer.name());
    timer.start();
  }
  ~TimeScope() { timer.stop(); }
  TimeScope(const TimeScope &) = delete;
  TimeScope &operator=(const TimeScope &) = delete;

  /// The nested --mlir-timing scope (empty without a parent): the parent
  /// of what runs inside this one, e.g. a PassManager's pass timers.
  mlir::TimingScope &timing() { return nested; }

private:
  Timer &timer;
  mlir::TimingScope nested;
};

/// The timers of one run (see the file comment).
class TimerGroup {
public:
  /// \p prefix: the key prefix (`vc` -> `vc.<name>-time`; empty:
  /// `<name>-time`). \p cpu: timers made by the group record CPU time.
  explicit TimerGroup(std::string prefix = "", bool cpu = false)
      : keyPrefix(std::move(prefix)), withCpu(cpu) {}
  /// As above, with the first lap starting at \p start (e.g. a
  /// Stopwatch's startTime()) instead of at a new clock read.
  TimerGroup(std::string prefix, SteadyClock::time_point start,
             bool cpu = false)
      : keyPrefix(std::move(prefix)), withCpu(cpu), lapStart(start) {}
  TimerGroup(const TimerGroup &) = delete;
  TimerGroup &operator=(const TimerGroup &) = delete;

  /// The timer named \p name, made at the first use. References stay
  /// valid for the life of the group.
  Timer &timer(llvm::StringRef name);
  /// The timer named \p name, null if never used.
  const Timer *find(llvm::StringRef name) const;

  /// The printed key of \p name: `<prefix>.<name>-time`.
  std::string key(llvm::StringRef name) const;

  /// Charges the wall time since the previous lap (or construction, or
  /// restartLap()) to timer(\p name): one clock read.
  void lap(llvm::StringRef name);
  /// Starts the next lap now, without charging the time since the last.
  void restartLap();

  /// Calls \p f on every timer in first-use order, with its key.
  void
  report(llvm::function_ref<void(llvm::StringRef key, const Timer &)> f) const;

  size_t size() const { return timers.size(); }
  bool empty() const { return timers.empty(); }

private:
  std::string keyPrefix;
  bool withCpu;
  /// first-use order; deque: references stay valid as timers are added
  std::deque<Timer> timers;
  SteadyClock::time_point lapStart = wallNow();
  double lapCpu = withCpu ? cpuSecondsNow() : 0;
};

} // namespace sea

#endif // SEA_SUPPORT_TIMER_H
