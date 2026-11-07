// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
extern unsigned nd(void);
extern void __VERIFIER_assert(int);
int main(void) {
  unsigned value = nd();
  unsigned next = value + 1;
  __VERIFIER_assert(next - 1 == value);
  return 0;
}
