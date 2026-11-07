// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
extern unsigned nd(void);
#include "seahorn/seahorn.h"
int main(void) {
  unsigned value = nd();
  sassert(value == 0);
  return 0;
}
