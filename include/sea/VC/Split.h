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
//   - Leaves: an address or integer (iN, N > 1) value whose term is an ite
//     tree (an address: or ptr-from-bits of one) over leaves: one case per
//     distinct leaf l, the value replaced by l (ptr-from-bits(l)), with the
//     disjunction of the path conditions of l's occurrences as the part
//     condition.
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
#include "sea/Expr/BaseRegistry.hh"
#include "sea/Expr/Expr.hh"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace sea {

class SeaOpSem;
class OpSemContext;

/// The type of a --split-leaves target: an address, or an integer wider
/// than i1 (an i1 value is a --split target).
bool isLeavesType(mlir::Type t);

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
/// of an address type or an integer type wider than i1) with a file
/// position (the op's own or an inlined call site) in a file whose path
/// ends with <file>, on <line>. A selector must find exactly one target.
/// Fails with `error` set.
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
  /// `isAddr`: the value is an address (leaf texts name objects); else an
  /// integer (leaf texts are numerals, unsigned, or the leaf's operator).
  static bool leafCases(expr::Expr t, SeaOpSem &sem, unsigned maxPaths,
                        std::vector<SplitCase> &out, std::string &why,
                        bool isAddr = true);

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

/// An address value whose term is an ite tree (or ptr-from-bits of one): a
/// --split-leaves candidate of a pick run (SplitCensus).
struct SplitAddrCandidate {
  mlir::Value value;
  /// The term as the encoding built it.
  expr::Expr term;
  /// Its leaf cases (SplitRun::leafCases); empty when it has more paths
  /// than the limit.
  std::vector<SplitCase> cases;
  /// Open alias questions whose pointers contain the term or one of its
  /// leaves without an object.
  uint64_t open = 0;
  /// Of those, the questions attributed to no other candidate.
  uint64_t own = 0;
  /// Of those, the questions whose pointers contain the term itself (not
  /// only an unknown leaf of it).
  uint64_t direct = 0;
  /// Distinct leaves without an object (cases SeaM cannot place).
  unsigned unknownLeaves = 0;
  /// --split-candidates: array reads of the query at the solver boundary
  /// whose index contains the term (SplitCensus::attributeReads).
  uint64_t reads = 0;
};

/// An i1 value or a block guard: a --split candidate of a pick run.
struct SplitBoolCandidate {
  /// Exactly one of value / block.
  mlir::Value value;
  mlir::Block *block = nullptr;
  /// The terms that stand for it in the query: the guard constant; for a
  /// value its bv1 term and that term = 1 (normalized).
  expr::ExprVector keys;
  /// Open alias questions with the target in an ite condition of their
  /// pointers or of the ite tree of an address candidate attributed to
  /// them.
  uint64_t open = 0;
  /// Address candidates with the target in an ite condition of their tree.
  unsigned addrs = 0;
  /// --split-candidates: array reads of the query at the solver boundary
  /// with the target in an ite condition of their index
  /// (SplitCensus::attributeReads).
  uint64_t reads = 0;
  /// Of those, the reads with an ite whose condition is the target itself
  /// (not only a part of it).
  uint64_t condReads = 0;
  /// The other Boolean candidates in its Boolean structure (its
  /// connectives, down to the atoms; attributeReads), by distinct term.
  unsigned covers = 0;
  /// --split-candidates: the arithmetic nodes of the query at the solver
  /// boundary (SplitCensus::attributeArith) with the target in an ite
  /// condition of an operand: all, multiplications (bvmul and the
  /// multiplication overflow predicates), divisions and remainders.
  uint64_t arith = 0, arithMul = 0, arithDiv = 0;
  /// The candidate (index in boolCands) listed for the term when another
  /// one has the same term, or -1 (attributeReads; the one with the most
  /// uses that reach a select condition, a branch condition or an address
  /// through i1 connectives, then the first).
  int sameAs = -1;
};

/// An integer (iN, N > 1) value whose term is an ite tree: a --split-leaves
/// candidate of --split-candidates, ranked by the arithmetic it controls.
struct SplitIntCandidate {
  mlir::Value value;
  /// The term as the encoding built it.
  expr::Expr term;
  /// Distinct leaves of its ite tree (0: more paths than the limit).
  unsigned leaves = 0;
  /// The arithmetic nodes of the query at the solver boundary with, in an
  /// operand, the term or an ite whose condition's atoms are all atoms of
  /// the conditions of the term's (normalized) ite tree: all,
  /// multiplications, divisions and remainders (SplitCensus::attributeArith);
  /// of all, those with the term itself in an operand.
  uint64_t arith = 0, arithMul = 0, arithDiv = 0, arithDirect = 0;
  /// The other integer candidates whose term (as encoded) holds this one's:
  /// the merged values computed from it, which its split fixes too.
  unsigned feeds = 0;
};

/// The kind of an arithmetic node for the arithmetic attribution and
/// --ite-stats: Mul (bvmul, the multiplication overflow predicates), Div
/// (bvudiv, bvsdiv, bvurem, bvsrem, bvsmod), or None.
enum class ArithKind { None, Mul, Div };
ArithKind arithKind(const expr::ENode *n);

/// The census of a solverless pick run (sea-vc --split-auto,
/// --split-candidates): the candidates the encoding defines (hooks), then
/// the attribution of the open alias questions of the normalized query to
/// them. A heuristic: one question counts for every candidate behind it.
class SplitCensus {
public:
  /// `full` (--split-candidates): also collect the Boolean candidates (i1
  /// values, guards) and the integer ones, and count `direct`.
  explicit SplitCensus(bool full) : bools(full), direct(full) {}
  /// Installs the hooks on `ctx`; they record and change nothing.
  void install(SeaOpSem &sem, OpSemContext &ctx);
  /// Attributes the open questions (IS_OVERLAP nodes of the normalized
  /// query) to the candidates.
  void attribute(SeaOpSem &sem, llvm::ArrayRef<expr::ENode *> openOverlaps,
                 unsigned maxPaths);
  /// Attributes the array reads (SELECT nodes with a non-numeral index) of
  /// `solverQuery`, the query at the solver boundary (SeaOpSem::solverTerm,
  /// before the Ackermann lowering, which turns the reads of read-only
  /// arrays into constants and pairs their indices): a Boolean candidate
  /// counts a read when it is in an ite condition of the read's index, an
  /// address candidate when its term is in the index (keys translated by
  /// solverTerm). Returns the number of reads. A heuristic for queries
  /// whose residue is index pairs, not open alias questions; call after
  /// attribute().
  uint64_t attributeReads(SeaOpSem &sem, const expr::Expr &solverQuery);
  /// Attributes the arithmetic nodes (arithKind) of `solverQuery` (as for
  /// attributeReads): a Boolean candidate counts a node when it is in an ite
  /// condition of one of its operands, an integer candidate when its term
  /// is in one of its operands, or an ite whose condition is over the atoms
  /// of the term's ite conditions (the normalization may fold a compare of
  /// the term into its conditions: `ite(g, 0, 1) = 0` to `g`). Also computes
  /// the integer candidates' leaves and `feeds`. Returns the number of distinct
  /// arithmetic nodes. A heuristic for queries whose cost is products or
  /// quotients by path-merged values; call after attributeReads() (it sets
  /// sameAs).
  uint64_t attributeArith(SeaOpSem &sem, const expr::Expr &solverQuery,
                          unsigned maxPaths);
  /// In encoding order.
  std::vector<SplitAddrCandidate> addrs;
  std::vector<SplitBoolCandidate> boolCands;
  /// Only with `full`.
  std::vector<SplitIntCandidate> ints;

private:
  bool bools, direct;
};

/// The census of sea-vc --ite-stats: the values whose terms are ite trees,
/// and the loads whose address term is not ground, recorded as the
/// encoding defines them (hooks; they change nothing and chain to the hooks
/// installed before them). Kinds of values: `mem` (memory values), `addr`
/// (addresses: an ite tree or ptr-from-bits of one), `int` (integers in the
/// backward slice of an arithmetic op's operands: mul, udiv, sdiv, urem,
/// srem, umulo, smulo, sdivo, through integer and i1 ops and block
/// arguments, not through memory). Kinds of load addresses: `ite` (an ite
/// tree, or ptr-from-bits of one), `from-bits` (ptr-from-bits of anything
/// else: a pointer read back from memory), `ite-index` (an ite below the
/// top: an index chosen by a path or a select), `symbolic` (any other term
/// with a free constant); a ground address (numerals and object bases) is
/// counted, not listed.
class IteCensus {
public:
  explicit IteCensus(ProgramOp program);
  void install(SeaOpSem &sem, OpSemContext &ctx);

  struct Value {
    const char *kind;
    mlir::Value value;
    /// Distinct leaves of the tree (DAG), distinct ite nodes, distinct
    /// conditions, and the block guards among the atoms of the conditions.
    unsigned leaves = 0, ites = 0, conds = 0, guardAtoms = 0;
  };
  struct Load {
    const char *kind;
    mlir::Operation *op;
  };
  std::vector<Value> values;
  /// Loads at non-ground addresses.
  std::vector<Load> loads;
  /// All loads the encoding met.
  uint64_t allLoads = 0;

private:
  /// No free constant but object bases (--seam's obj.<name>).
  bool ground(expr::ENode *n, const expr::BaseRegistry &bases);
  llvm::DenseSet<mlir::Value> feedsArith;
  std::set<expr::ENode *> guards;
  /// An ite in the DAG.
  bool hasIte(expr::ENode *n);
  std::unordered_map<expr::ENode *, bool> groundMemo, iteMemo;
  /// Keeps the guard terms alive (the set holds raw pointers).
  expr::ExprVector keep;
};

} // namespace sea

#endif // SEA_VC_SPLIT_H
