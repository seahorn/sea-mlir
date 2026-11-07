// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "seahorn/seahorn.h"
extern unsigned nd(void);
int main(void) {
  unsigned first = nd();
  unsigned second = nd();
  assume(first <= 100);
  assume(second <= 100);
  sassert(first + second <= 200);
  return 0;
}
