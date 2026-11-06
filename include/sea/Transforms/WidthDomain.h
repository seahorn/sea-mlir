// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- WidthDomain.h - when a wide byte-memory access is one width term -===//
//
// docs/design/SeaM-mixed-width.md: a load / store of an exact byte memory
// (i8 cells, byte addresses, no word layout) of `bits` bits is kept whole
// as a width term only inside the width domain 0 < bytes <= 2^n (n = index
// bits: one access does not revisit an address). Shared by the lowering
// (sea-split-wide-access{keep-byte-wide}) and the VC entry (SeaOpSem), so
// that a program that reaches the VC without the lowering is held to the
// same contract: an oversized access gave a false
// UNSAT without --lower.
//
//===----------------------------------------------------------------------===//
#ifndef SEA_TRANSFORMS_WIDTHDOMAIN_H
#define SEA_TRANSFORMS_WIDTHDOMAIN_H

#include "sea/Dialect/Sea/SeaOps.h"

namespace sea {

inline bool inWideByteDomain(MemType memTy, unsigned bits) {
  if (memTy.isWordLayout() || memTy.getCell().getWidth() != 8 ||
      memTy.getCellUnits() != 1 || bits <= 8 || bits % 8 != 0)
    return false;
  unsigned n = memTy.getIndexWidth();
  return n >= 32 || uint64_t(bits / 8) <= (uint64_t(1) << n);
}

} // namespace sea

#endif
