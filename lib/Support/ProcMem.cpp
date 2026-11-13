// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ProcMem.cpp - the memory use of this process -------------*- C++ -*-===//

#include "sea/Support/ProcMem.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace sea {

#ifdef __linux__
/// Parses the first two decimal fields of /proc/self/statm (pages): the
/// address-space size and the resident size.
static bool readStatm(uint64_t &vmPages, uint64_t &rssPages) {
  int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  char buf[128];
  ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  ::close(fd);
  if (n <= 0)
    return false;
  buf[n] = 0;
  uint64_t v[2] = {0, 0};
  const char *p = buf;
  for (uint64_t &x : v) {
    while (*p == ' ')
      ++p;
    if (*p < '0' || *p > '9')
      return false;
    while (*p >= '0' && *p <= '9')
      x = x * 10 + uint64_t(*p++ - '0');
  }
  vmPages = v[0];
  rssPages = v[1];
  return true;
}
#endif

ProcMem readProcMem() {
  ProcMem m;
#ifdef __linux__
  uint64_t vm, rss;
  if (readStatm(vm, rss)) {
    const uint64_t pageKB = uint64_t(::sysconf(_SC_PAGESIZE)) / 1024;
    m.vmKB = vm * pageKB;
    m.rssKB = rss * pageKB;
    m.hasCurrent = true;
  }
#endif
  struct rusage ru;
  if (::getrusage(RUSAGE_SELF, &ru) == 0) {
#ifdef __APPLE__
    m.peakRssKB = uint64_t(ru.ru_maxrss) / 1024; // bytes
#else
    m.peakRssKB = uint64_t(ru.ru_maxrss); // KiB (Linux, BSDs)
#endif
    m.hasPeak = true;
  }
  return m;
}

} // namespace sea
