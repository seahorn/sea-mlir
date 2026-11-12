// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Split.h - case splitting of a VC (sea-vc --split) --------*- C++ -*-===//
//
// Expression-level case splitting of the VC by Shannon cofactoring, before
// SeaM rewrites the uses of the split value (docs/design/Split.md). A split
// target is a value or block of the encoded program:
//   - Bool: an i1 value v with term t: case b in {1, 0} replaces v by the
//     numeral b, with the part condition t = b; a block guard g: case b in
//     {true, false} replaces g by b, with the condition g = b;
//   - Leaves: a value whose term is an ite tree (or ptr-from-bits of one)
//     over leaves: one case per distinct leaf l, the value replaced by l
//     (ptr-from-bits(l)), with the disjunction of the path conditions of
//     l's occurrences as the part condition.
// The replacement happens when the target is defined during the encoding
// (OpSemContext::defHook / guardHook), so every later use, and SeaM's
// rewriting of it, sees the fixed case. For every choice of cases the part
// is VC[t := value] /\ condition; the conditions of the cases of a target
// are exhaustive and each implies t = value, so the original query is
// unsat iff every part is (checked per run by sea-vc --split-check).
//
// Nested targets: one level per target the encoding meets (program order),
// each with its own cases under the cases chosen before it. A run is given
// the choices of the first levels (the prefix); deeper levels take case 0
// and report their case counts, so the parts are enumerated depth-first
// with one encoding run per part (sea-vc).
//
//===----------------------------------------------------------------------===//

#ifndef SEA_VC_SPLIT_H
#define SEA_VC_SPLIT_H

#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Expr/Expr.hh"

#include "llvm/ADT/DenseMap.h"

#include <string>
#include <vector>

namespace sea {

class SeaOpSem;
class OpSemContext;

/// A value or block to split on, found by a selector.
struct SplitTarget {
  enum class Kind { Bool, Leaves };
  Kind kind;
  /// Exactly one of value / block.
  mlir::Value value;
  mlir::Block *block = nullptr;
  /// The VC name of the value / block (ValueNames) and the selector that
  /// found it.
  std::string name, selector;
};

/// Finds the targets of `selectors` in `program` (the program the VC
/// encodes, after the lowering). A selector is a VC name (`%x`, `nd.<site>`,
/// `^bb3`) or `@<file>:<line>`: the op results (Bool: of type i1; Leaves:
/// of an address type) with a file position (the op's own or an inlined
/// call site) in a file whose path ends with <file>, on <line>. A selector
/// must find exactly one target. Fails with `error` set.
mlir::LogicalResult resolveSplitTargets(ProgramOp program,
                                        llvm::ArrayRef<std::string> selectors,
                                        SplitTarget::Kind kind,
                                        std::vector<SplitTarget> &out,
                                        std::string &error);

/// One case of a level: the value the target takes, and the condition
/// under which the target's term equals it.
struct SplitCase {
  expr::Expr value;
  expr::Expr cond;
  /// Short text for reports (`true`, `leaf obj.x`, ...).
  std::string text;
};

/// A target met by the encoding, with its cases and the one chosen.
struct SplitLevel {
  unsigned target;
  /// The target's term before the split (the guard constant for a block).
  expr::Expr term;
  std::vector<SplitCase> cases;
  unsigned chosen = 0;
  /// The value is a pointer (the exactness check compares by PTR_EQ).
  bool ptr = false;
};

/// The split state of one encoding run.
class SplitRun {
public:
  SplitRun(const std::vector<SplitTarget> &targets,
           std::vector<unsigned> prefix, unsigned maxTreePaths)
      : targets(targets), prefix(std::move(prefix)),
        maxTreePaths(maxTreePaths) {
    for (unsigned i = 0; i < targets.size(); ++i) {
      if (targets[i].block)
        byBlock[targets[i].block] = i;
      else
        byValue[targets[i].value] = i;
    }
  }

  /// Test only (sea-vc --x-split-plant): a broken construction that
  /// --split-check must catch. DropCase: a Leaves level loses its last
  /// case (not exhaustive); WrongValue: the chosen case takes the next
  /// case's value (not exact).
  enum class Plant { None, DropCase, WrongValue };
  void setPlant(Plant p) { plant = p; }

  /// Installs the hooks on `ctx` (cleared by the context's owner).
  void install(SeaOpSem &sem, OpSemContext &ctx);

  /// The levels met so far, in encoding order.
  const std::vector<SplitLevel> &levels() const { return lvls; }
  /// The condition of the part: the conjunction of the chosen cases'
  /// conditions (null: no level).
  expr::Expr partCondition(expr::ExprFactory &efac) const;
  /// `<name>=<case text>[,...]` of the chosen cases.
  std::string choiceText() const;
  /// Empty, or why the encoding could not split (a target whose term has no
  /// cases of its kind; the run must stop).
  const std::string &failure() const { return why; }
  /// The failure is a plan the run could not follow (vc.split-unstable).
  bool unstable() const { return isUnstable; }
  /// Leaves levels whose chosen value passed the structural check (a leaf
  /// of the term's ite tree, or the rewriter's normal form of
  /// ptr-from-bits of one).
  unsigned leafStructural() const { return nStructural; }

  /// The cases of a Leaves target's term `t`: the ite tree under at most one
  /// PTR_FROM_BITS, by distinct leaf (first occurrence order). False (with
  /// `why`) when `t` is not of that form or has more than `maxPaths` paths.
  static bool leafCases(expr::Expr t, SeaOpSem &sem, unsigned maxPaths,
                        std::vector<SplitCase> &out, std::string &why);

private:
  void onValue(mlir::Value v, SeaOpSem &sem, OpSemContext &ctx);
  expr::Expr onGuard(mlir::Block *b, expr::Expr g, OpSemContext &ctx);
  unsigned choose(unsigned level, unsigned n);

  const std::vector<SplitTarget> &targets;
  std::vector<unsigned> prefix;
  unsigned maxTreePaths;
  Plant plant = Plant::None;
  llvm::DenseMap<mlir::Value, unsigned> byValue;
  llvm::DenseMap<mlir::Block *, unsigned> byBlock;
  std::vector<SplitLevel> lvls;
  std::string why;
  bool isUnstable = false;
  unsigned nStructural = 0;
};

} // namespace sea

#endif // SEA_VC_SPLIT_H
