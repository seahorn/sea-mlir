// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Assume.h - the ledger of named assumptions ---------------*- C++ -*-===//
//
// The named assumptions an answer rests on (conditional soundness): every
// `assume.<name>:` line of the tools comes from one Ledger, so a count
// cannot be dropped or misspelled on the way to the output.
//
//  Name    the named assumptions, in their printed order:
//            memset-bound        sea-expand-memset's bound (VC)
//            a-inb               the lowering's dropped memhavocs (VC)
//            addr0-is-null       address 0 is null (VC and runs)
//            memhavoc-null-noop  memhavoc(null) is a no-op (VC and runs)
//  Ledger  what one run / one VC / one report relied on:
//            use(name, n)          n steps of a run rested on it (summed;
//                                  a call with n > 0 counts one run)
//            site(name, field, n)  a fact of the VC: the field `field=n`
//                                  of its VC line (in call order)
//            report(r)             the lines, in Name order, per name the
//                                  VC line, then the uses line:
//                assume.<name>:[ vc] <field>=<n>...  (any site() call)
//                assume.<name>: uses=<n>             (n > 0)
//              the state word `vc` marks the VC line of the names runs
//              also count (addr0-is-null, memhavoc-null-noop). The lines'
//              BRUNCH_STAT form is Parts (`assume.<name>-<field> <n>`,
//              `assume.<name> vc`, `assume.<name>-uses <n>`) unless the
//              report's rule is Off.
//            runFields(os, runs)   the run names' counts as one k=v list,
//                                  `addr0-is-null=<n>[ addr0-is-null-runs=
//                                  <r>] memhavoc-null-noop=...` (corpus
//                                  headers, fuzz.assumptions)
//
// The seam.inb line (A-INB of --seam) is not here: its form is parsed by
// utils/query-diff.py and fuzz-seam and stays in sea-vc's SeaM block.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_SUPPORT_ASSUME_H
#define SEA_SUPPORT_ASSUME_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace sea {
namespace stats {
class Report;
} // namespace stats

namespace assume {

/// The named assumptions, in their printed order.
enum class Name : unsigned {
  MemsetBound,
  AInb,
  Addr0IsNull,
  MemhavocNullNoop,
};
constexpr unsigned kNames = 4;

/// `memset-bound`, `a-inb`, ...
llvm::StringRef nameOf(Name a);
/// Whether runs (the interpreter) count uses of \p a.
bool countedByRuns(Name a);

/// What one run / VC / report relied on (see the file comment).
class Ledger {
public:
  /// \p n steps of a run rested on \p a (summed; n > 0 counts one run).
  void use(Name a, uint64_t n);
  /// The uses of a run result (Interpreter's RunResult, a ReachHit: any
  /// type with addr0IsNullUses and memhavocNullNoopUses).
  template <typename RunT>
  void useRun(const RunT &r) {
    use(Name::Addr0IsNull, r.addr0IsNullUses);
    use(Name::MemhavocNullNoop, r.memhavocNullNoopUses);
  }
  /// A fact of the VC: `<field>=<n>` on the VC line of \p a.
  void site(Name a, llvm::StringRef field, uint64_t n);
  /// Adds the uses, runs and sites of \p o.
  void add(const Ledger &o);

  uint64_t uses(Name a) const { return at(a).uses; }
  uint64_t runs(Name a) const { return at(a).runs; }
  /// Whether some run used a run-counted name.
  bool anyUse() const;

  /// The lines (see the file comment), appended to \p r.
  void report(stats::Report &r) const;
  /// `<name>=<uses>[ <name>-runs=<runs>]` for each run-counted name,
  /// separated by blanks.
  void runFields(llvm::raw_ostream &os, bool withRuns) const;
  std::string runFields(bool withRuns) const;

private:
  struct Entry {
    uint64_t uses = 0, runs = 0;
    bool hasSites = false;
    llvm::SmallVector<std::pair<std::string, uint64_t>, 2> sites;
  };
  const Entry &at(Name a) const { return all[static_cast<unsigned>(a)]; }
  Entry &at(Name a) { return all[static_cast<unsigned>(a)]; }
  std::array<Entry, kNames> all;
};

} // namespace assume
} // namespace sea

#endif // SEA_SUPPORT_ASSUME_H
