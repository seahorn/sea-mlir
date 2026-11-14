// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Timer.cpp - wall (and CPU) time of named phases --------------------===//

#include "sea/Support/Timer.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <ctime>

using namespace sea;

std::atomic<FakeClock *> sea::detail::fakeClock{nullptr};

void FakeClock::advance(double wallSeconds, double cpuSeconds) {
  wall += std::chrono::duration_cast<SteadyClock::duration>(
      std::chrono::duration<double>(wallSeconds));
  cpu += cpuSeconds;
}

void sea::setFakeClock(FakeClock *clock) {
  detail::fakeClock.store(clock, std::memory_order_relaxed);
}

double sea::cpuSecondsNow() {
  if (FakeClock *f = detail::fakeClock.load(std::memory_order_relaxed))
    return f->cpu;
#if defined(CLOCK_PROCESS_CPUTIME_ID)
  timespec ts;
  if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#endif
  return double(std::clock()) / CLOCKS_PER_SEC;
}

std::string sea::secondsText(double seconds) {
  std::string s;
  llvm::raw_string_ostream(s) << llvm::format("%.3f", seconds);
  return s;
}

Timer &TimerGroup::timer(llvm::StringRef name) {
  for (Timer &t : timers)
    if (t.name() == name)
      return t;
  return timers.emplace_back(name.str(), withCpu);
}

const Timer *TimerGroup::find(llvm::StringRef name) const {
  for (const Timer &t : timers)
    if (t.name() == name)
      return &t;
  return nullptr;
}

std::string TimerGroup::key(llvm::StringRef name) const {
  if (keyPrefix.empty())
    return (llvm::Twine(name) + "-time").str();
  return (llvm::Twine(keyPrefix) + "." + name + "-time").str();
}

void TimerGroup::lap(llvm::StringRef name) {
  SteadyClock::time_point now = wallNow();
  double cpuNow = withCpu ? cpuSecondsNow() : 0;
  timer(name).add(secondsBetween(lapStart, now), cpuNow - lapCpu);
  lapStart = now;
  lapCpu = cpuNow;
}

void TimerGroup::restartLap() {
  lapStart = wallNow();
  lapCpu = withCpu ? cpuSecondsNow() : 0;
}

void TimerGroup::report(
    llvm::function_ref<void(llvm::StringRef key, const Timer &)> f) const {
  for (const Timer &t : timers)
    f(key(t.name()), t);
}
