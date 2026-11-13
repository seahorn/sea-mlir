// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- ProcMem.h - the memory use of this process ---------------*- C++ -*-===//
//
// The resident size, the peak resident size and the address-space size of
// the running process, for progress and phase reports (sea-vc vc.phase).
// readProcMem does not allocate (open/read on /proc, getrusage), so a
// new-handler may call it.
//
// Linux: current from /proc/self/statm, peak from getrusage(ru_maxrss).
// Elsewhere: the peak only (getrusage; bytes on macOS); the current sizes
// are unknown (has* false) and reports omit them.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_SUPPORT_PROCMEM_H
#define SEA_SUPPORT_PROCMEM_H

#include <cstdint>

namespace sea {

struct ProcMem {
  /// Resident set size and address-space (virtual) size, in KiB; valid when
  /// hasCurrent.
  uint64_t rssKB = 0, vmKB = 0;
  bool hasCurrent = false;
  /// Peak resident set size so far, in KiB; valid when hasPeak.
  uint64_t peakRssKB = 0;
  bool hasPeak = false;

  static uint64_t mb(uint64_t kb) { return (kb + 512) / 1024; }
};

/// The sizes now (see the file comment). Does not allocate.
ProcMem readProcMem();

} // namespace sea

#endif // SEA_SUPPORT_PROCMEM_H
