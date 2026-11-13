// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- OpSem.h - symbolic execution of sea programs -------------*- C++ -*-===//
//
// The symbolic-execution half of the VC generator (SeaHorn's split): SeaOpSem
// executes sea ops on a symbolic store and records side conditions; VCGen
// (VCGen.h) connects the stores of different blocks. Modeled on SeaHorn's
// OperationalSemantics / OpSemContext (include/seahorn/OperationalSemantics.hh,
// dev18 0cb925cd).
//
// i1 is a bv1 here. Where the VC needs a Bool (branches, assume, assert) it
// converts with toBool; icmp produces a bv1 with fromBool. These two
// functions are the only places that decide the i1 encoding.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_VC_OPSEM_H
#define SEA_VC_OPSEM_H

#include "sea/Dialect/Sea/SeaOps.h"
#include "sea/Expr/BaseRegistry.hh"
#include "sea/Expr/Expr.hh"
#include "sea/Expr/RewriteTrail.hh"
#include "sea/VC/JoinCases.h"
#include "sea/VC/Names.h"
#include "sea/VC/SymStore.hh"

#include "llvm/ADT/SmallVector.h"

#include <functional>
#include <memory>
#include <optional>

namespace expr {
class SeaMRewriter;
class PtrIndexExpander;
struct SeaMOptions;
struct RewriteStats;
} // namespace expr

namespace sea {

/// State of a symbolic execution: the store, the side conditions collected
/// so far, the path condition that scopes new facts, and the guarantees
/// (assertions) to be checked.
class OpSemContext {
public:
  OpSemContext(SymStore &values, expr::ExprVector &side);

  SymStore &values() { return store; }
  expr::Expr read(expr::Expr reg) { return store.read(reg); }
  expr::Expr havoc(expr::Expr reg) { return store.havoc(reg); }
  void write(expr::Expr reg, expr::Expr val) {
    store.write(reg, writeHook ? writeHook(reg, val) : val);
  }
  /// Applied to every value written to a register (op results and block
  /// arguments): the per-write simplification of the lambda memory encoding
  /// (docs/design/Lambda.md). Null: none.
  std::function<expr::Expr(expr::Expr reg, expr::Expr val)> writeHook;
  /// Called after the op defining `v` is encoded (SeaOpSem::exec), and
  /// after the block arguments of a block are joined (VCGen), with the
  /// value in the store: case splitting (sea/VC/Split.h) may replace it.
  /// Null: none.
  std::function<void(mlir::Value v)> defHook;
  /// Applied to the guard constant of every block but the entry when VCGen
  /// creates it; the result is the block's guard (case splitting). Null:
  /// none.
  std::function<expr::Expr(mlir::Block *b, expr::Expr guard)> guardHook;

  OpSemContext &pc(expr::Expr cond) {
    pathCond = cond;
    return *this;
  }
  void setPathCond(expr::Expr cond) { pathCond = cond; }
  expr::Expr getPathCond() const { return pathCond; }

  expr::ExprVector &side() { return sideConds; }
  void addSide(expr::Expr e) { sideConds.push_back(e); }
  /// Adds `pathCond => e`.
  void addScopedSide(expr::Expr e);

  /// Adds `pathCond => e` as a property to check.
  void addScopedGuarantee(expr::Expr e);
  const expr::ExprVector &guarantees() const { return guaranteeConds; }
  expr::ExprVector &guarantees() { return guaranteeConds; }

  expr::ExprFactory &efac() const { return store.getExprFactory(); }

private:
  SymStore &store;
  expr::ExprVector &sideConds;
  expr::ExprVector guaranteeConds;
  expr::Expr pathCond;
};

/// Bit-vector semantics of sea ops over Expr. Must agree with
/// Dialect/Sea/Semantics.cpp (checked by sea-vc's replay validation and the
/// oracles). Memory uses the array encoding (docs/design/Memory.md): load
/// and store; memset, memmove and memhavoc with a constant length become
/// exact chains of stores (as SeaHorn's array representation does for
/// concrete lengths); a symbolic length is refused.
class SeaOpSem {
public:
  /// With \p nativeMulOverflow, sea.umulo / sea.smulo are encoded with the
  /// solver's overflow predicates (z3, Bitwuzla); otherwise they must be
  /// expanded.
  ///
  /// With \p seam (SeaM, docs/design/SeaM.md), a sea.segment result is the
  /// named constant `obj.<name>` with the side constraint `obj.<name> =
  /// <base>`, and the objects are recorded in baseRegistry(). Without it the
  /// result is the numeral base itself: the VC has the shape it had before
  /// segments had results.
  ///
  /// With \p seam every load and store is also rewritten as it is encoded
  /// (include/sea/Expr/SeaMRules.hh): one rewriter and one cache per
  /// SeaOpSem (that is, per VC), configured by \p seamOpts (null: defaults).
  /// With seamOpts->simplify the rewriter exists without \p seam
  /// as well, and only normalizes (simplify(): no memory rules, no
  /// assumption).
  SeaOpSem(expr::ExprFactory &efac, ValueNames &names,
           bool nativeMulOverflow = false, bool seam = false,
           const expr::SeaMOptions *seamOpts = nullptr);
  ~SeaOpSem();

  bool seam() const { return seamMode; }
  /// The pointer mode (sea-vc --ptr-mode, PtrSort rev 5 S0), read at the
  /// encoding site (S2f): with SeaM, an addr.from_int is PTR_FROM_INT in
  /// address mode (counted, ptrFromIntOpaque) and the error vc.int-to-ptr in
  /// opaque mode (the tool refuses such a program before; here it is a bug).
  void setAddressMode(bool on) { addressMode = on; }
  bool address() const { return addressMode; }
  /// addr.from_int sites encoded as PTR_FROM_INT: a pointer with no
  /// provenance (D17, no reprovenance), opaque to the object rules.
  unsigned ptrFromIntOpaque() const { return numPtrFromInt; }
  /// PtrSort S6 (rev 5 2.4): the pointer companion, opaque mode with SeaM
  /// (sea-vc --ptr-companion). Beside each pointer-indexed memory value M
  /// an Expr-only memory M.ptr : array(ptr(w), ptr(w)) holds the pointer of
  /// a pointer store at its address: written at a pointer store (whole, or
  /// the offset-0 byte of a split one), copied by a memmove (mcopy), kept
  /// by memset / memhavoc and integer stores (a stale cell is harmless: see
  /// below), joined as an ite (joinCompanions), fresh where nothing is
  /// known (a memory constant, an unrecognized producer). A split pointer
  /// load from_bits(bits) at q of M is
  ///   ite(to_bits(c) = bits, c, from_bits(bits)),  c = select(M.ptr, q)
  /// (ptr.load-resolve-split): address-exact whatever c is (the guard), and
  /// c when the bytes are the stored pointer's.
  /// A-PTRINT (rev 4 D11, a named premise, off unless asked): "a pointer
  /// loaded from memory is the one last stored there": the split load is c,
  /// no guard (counted, companionAssumed; sea-vc reruns a sat answer
  /// without it).
  void setPtrCompanion(bool on, bool assume = false, bool check = false) {
    ptrCompanionOn = on;
    ptrIntAssume = assume;
    ptrIntCheck = check;
  }
  /// The discharge of A-PTRINT (--ptr-companion=check): the exact encoding,
  /// the guard of every split pointer load is the property and the
  /// program's assertions are not (unsat: A-PTRINT holds on every execution,
  /// under A-INB).
  bool ptrIntChecking() const { return ptrCompanion() && ptrIntCheck; }
  unsigned companionChecked() const { return numCompanionChecked; }
  unsigned companionAssumed() const { return numCompanionAssumed; }
  bool ptrCompanion() const {
    return ptrCompanionOn && seamMode && ptrSorted() && !addressMode;
  }
  /// the companions of \p dst's memory arguments: the ite over \p edges of
  /// the incoming companions (VCGen's ite join); with \p cases
  /// (--vc-join-cases) built through a cases term. Fails (reported) only
  /// when \p cases refuses a join.
  mlir::LogicalResult joinCompanions(mlir::Block &dst,
                                     llvm::ArrayRef<mlir::Block *> preds,
                                     llvm::ArrayRef<expr::Expr> edges,
                                     OpSemContext &ctx,
                                     JoinCases *cases = nullptr);
  unsigned companionLoads() const { return numCompanionLoads; }
  unsigned companionStores() const { return numCompanionStores; }
  unsigned companionCopies() const { return numCompanionCopies; }
  unsigned companionFresh() const { return numCompanionFresh; }
  /// The lambda memory encoding (docs/design/Lambda.md, sea-vc
  /// --mem=lambda): a memory value is a unary function `I -> C` (a LAMBDA, an
  /// ite of such, or the coercion `lambda i. select(A, i)` of an array
  /// constant); a load is `fapp(M, p)`; stores and memset / memmove /
  /// memhavoc build lambdas with the circular membership `(i - d) <u n`.
  /// Byte (index-unit) memories only. Not with seam.
  void setLambdaMemory(bool on) { lambdaMode = on; }
  bool lambdaMemory() const { return lambdaMode; }
  /// Is an address a pointer, ptr(w) (PtrSort, D5; S2e with SeaM, S2g
  /// without)? In every VC but the lambda encoding: there z3 simplifies
  /// every written value, and z3 has no pointer sort.
  bool ptrSorted() const { return !lambdaMode; }
  /// Does \p e hold a pointer-sorted term (ptr(w), a node, a marker,
  /// PTR_EQ)? After solverTerm none may be left (vc.ptr-sort-at-boundary).
  bool hasPtrTerms(const expr::Expr &e);
  /// Lambda encoding: \p v (a memory value, possibly as returned by the z3
  /// simplifier: a lambda, an ite of memories, an array term) in the
  /// functional representation; \p reg is the memory register (its array
  /// constant gives the index sort). Null if \p v is not a memory.
  expr::Expr coerceMemory(expr::Expr v, expr::Expr reg);
  /// Is \p reg the register of a memory value (an array constant)?
  static bool isMemoryReg(expr::Expr reg);
  /// Lambda encoding (docs/design/Lambda.md, well-scoped terms): does \p e
  /// mention a bound variable not bound by an enclosing LAMBDA in \p e?
  static bool hasFreeBoundVar(expr::Expr e);
  /// The object bases seen so far (empty unless seam()); in allocation order.
  const expr::BaseRegistry &baseRegistry() const { return bases; }
  /// The per-rule counters of the rewriter (null unless seam() or
  /// simplifies()).
  const expr::RewriteStats *seamStats() const;
  /// Seconds spent rewriting loads and stores so far (0 unless seam()): the
  /// pre-processing of the thesis' Table 8.4.
  double seamRewriteSeconds() const;
  /// True if simplify() normalizes (SeaMOptions::simplify).
  bool simplifies() const { return simplifying; }
  /// The normal form of a VC term (a side condition, the query) under the
  /// bit-vector and Boolean rules of BvRules.hh, with the cache of the load
  /// and store rewriting; \p e itself unless simplifies().
  expr::Expr simplify(expr::Expr e);
  /// Seconds spent in simplify() so far.
  double simplifySeconds() const;
  /// \p e as a solver (or a model evaluation) can take it: with SeaM, every
  /// store-map expanded into stores (include/sea/Expr/StoreMap.hh); \p e
  /// itself otherwise. The query and every model evaluation must go through
  /// it: memory values of the VC may hold store-maps, which are not SMT terms.
  /// Null (with the error vc.memop-term-at-boundary) if a memory-operation
  /// term is in a position that is not read (MemOps.hh).
  expr::Expr solverTerm(expr::Expr e);
  /// solverTerm without the diagnostic: null and \p why on a refusal.
  expr::Expr solverTerm(expr::Expr e, std::string &why);
  /// the negated firing `lhs != rhs` of \p f as the solver gets it
  /// (both sides through solverTerm); a memory-valued firing over a
  /// memory-operation term compares the two memories at a fresh index
  /// (extensionality). Null (\p why) if a side cannot be translated.
  expr::Expr firingObligation(const expr::RewriteTrail::Firing &f,
                              std::string &why);

  expr::ExprFactory &efac() const { return ef; }

  /// The rule trail (include/sea/Expr/RewriteTrail.hh): every rule
  /// firing of the rewriter is recorded in \p t, with the operation being
  /// encoded as its context. Null: off. No effect without a rewriter.
  void setTrail(expr::RewriteTrail *t);
  /// The context of the next firings when they do not come from an op: a
  /// side condition ("side") or the query ("query").
  void setTrailContext(llvm::StringRef kind);
  /// the premise under which the firing \p f must be valid
  /// (`premise => lhs = rhs`): true for a firing without premises; for one
  /// that uses A-INB, the layout (obj.k = base for every object) and the
  /// in-bounds condition of each access address the rule relies on (
  /// Rules::accessAddresses, at the access size of the firing's context);
  /// for one that uses LAYOUT only (a shadow memory), the layout.
  /// Null if the premise cannot be expressed (\p why says why). This states
  /// what the rule claims to rest on; it does not discharge A-INB.
  expr::Expr firingPremise(const expr::RewriteTrail &trail,
                           const expr::RewriteTrail::Firing &f,
                           std::string &why);

  /// The A-INB premise of the accesses a firing at \p f's lhs looks at,
  /// whatever premises the rule used: the layout, the in-bounds condition of
  /// each access address (Rules::accessAddresses, at the access size of the
  /// firing's context) and of each range (Rules::accessRanges). For the
  /// equality audit (sea-vc --eq-audit, level L2): would A-INB have decided
  /// what the rule left undecided? Null (\p why) if it cannot be stated.
  expr::Expr accessPremise(const expr::RewriteTrail &trail,
                           const expr::RewriteTrail::Firing &f,
                           std::string &why);
  /// The layout: obj.k = base for every object (a fact of the VC).
  expr::Expr layoutFacts();

  /// The symbolic register of a value (a bit-vector constant of its width)
  /// or of a block (a Bool constant: the block executes).
  expr::Expr mkSymbReg(mlir::Value v);
  expr::Expr mkSymbReg(mlir::Block *b);

  /// The current value of `v` in the store.
  expr::Expr lookup(mlir::Value v, OpSemContext &ctx) {
    return ctx.read(mkSymbReg(v));
  }

  /// Executes the ops of `b` except its terminator. Facts are scoped by the
  /// context's path condition. Fails with a diagnostic on an op it does not
  /// support.
  mlir::LogicalResult exec(mlir::Block &b, OpSemContext &ctx);

  /// Adds `pathCond => <condition under which src branches to dst>`.
  void execBr(mlir::Block &src, mlir::Block &dst, OpSemContext &ctx);

  /// The values src's terminator passes to dst's arguments.
  llvm::SmallVector<expr::Expr> execPhi(mlir::Block &dst, mlir::Block &src,
                                        OpSemContext &ctx);

  /// A bit-vector numeral of v's width.
  expr::Expr numeral(const llvm::APInt &v);

  /// The fresh array holding the havocked cells of a sea.memhavoc (named
  /// nd.<site>), or null if `op` was not executed.
  expr::Expr havocArray(MemHavocOp op) const { return havocs.lookup(op); }
  /// The initial array of a memory site (lambda encoding: the register holds
  /// its coercion `lambda i. select(A, i)`; null in the array encoding,
  /// where the register value is the array itself).
  expr::Expr initialArray(MemNondetOp op) const { return havocs.lookup(op); }

  expr::Expr toBool(expr::Expr bv1);
  expr::Expr fromBool(expr::Expr b);

  /// A-INB bookkeeping (with SeaM): the accesses seen, the ones with an
  /// address leaf that is definitely out of its object (numeral offset), and
  /// the distinct accesses whose in-bounds condition is not constant. A-INB
  /// itself is assumed, not checked.
  unsigned inbAccessCount() const { return inbAccesses; }
  unsigned inbStaticViolationCount() const { return inbStaticViolations; }
  unsigned inbConditionCount() const { return inbSeen.size(); }
  /// The first kInbViolationNotes definite violations, as `<file:line:col>
  /// <kind>: <reason>` (sea-vc prints them with vc.seam-inb-violated).
  static constexpr unsigned kInbViolationNotes = 8;
  const std::vector<std::string> &inbViolationNotes() const { return inbNotes; }
  /// EXPERIMENT (sea-vc --x-read-table): the rewriter, and the A-INB
  /// violation terms `path && !inb` of the accesses seen.
  expr::SeaMRewriter *seamRewriter() { return seamRw.get(); }
  const expr::ExprSet &inbViolationTerms() const { return inbSeen; }
  /// EXPERIMENT: positions in the side conditions of the sea.assume facts.
  const std::vector<size_t> &assumeSidePositions() const { return assumeSides; }

private:
  expr::Expr bvnum(const llvm::APInt &v);
  /// Encodes memset / memmove / memhavoc with a constant length; fails with
  /// a diagnostic on a symbolic or too large length.
  mlir::LogicalResult execRangeOp(mlir::Operation &op, OpSemContext &ctx);
  /// With SeaM, a range op of a byte memory as one term (MemOps.hh).
  void execRangeTerm(mlir::Operation &op, mlir::Value len, OpSemContext &ctx);
  /// A-INB bookkeeping of the range [d, d + n) of a memory operation.
  void accessRange(expr::Expr d, expr::Expr n, OpSemContext &ctx);

  /// select / store as the VC encodes them: with SeaM, rewritten.
  /// \p units is the size of the access in address units.
  expr::Expr memLoad(expr::Expr mem, expr::Expr ptr, uint64_t units,
                     OpSemContext &ctx);
  expr::Expr memStore(expr::Expr mem, expr::Expr ptr, expr::Expr val,
                      uint64_t units, OpSemContext &ctx);
  /// The address as the access uses it (with SeaM, normalized); records the
  /// A-INB condition of the access, unless it is an access of a shadow
  /// memory (\p mem; SeaM-null.md, "Shadow memories": keyed by object
  /// identity, not a program access, so A-INB says nothing about it).
  expr::Expr accessAddr(expr::Expr mem, expr::Expr ptr, uint64_t units,
                        OpSemContext &ctx);

  /// the trail context of a memory access of the op being executed
  void trailAccess(const char *kind, uint64_t units);
  /// the op exec() is executing (the trail's context)
  mlir::Operation *curOp = nullptr;

  llvm::DenseMap<mlir::Operation *, expr::Expr> havocs;
  /// mkSymbReg's results (one Expr per value / block for the VC).
  llvm::DenseMap<mlir::Value, expr::Expr> symbRegs;
  llvm::DenseMap<mlir::Block *, expr::Expr> blockRegs;
  expr::BaseRegistry bases;
  /// the start terms of the memories of sea.mem.const without `shadow`
  /// (data): one that is also a shadow root is reported (vc.seam-shadow-
  /// shared-start)
  expr::ExprSet dataConstRoots;
  /// the program's shadow starts were registered (exec, first call)
  bool shadowScanned = false;
  /// registers a shadow memory start (BaseRegistry::addShadowRoot)
  void addShadowRoot(expr::Expr start);
  unsigned inbAccesses = 0, inbStaticViolations = 0;
  std::vector<std::string> inbNotes;
  /// counts a definite A-INB violation; notes the first ones (\p loc, the
  /// access \p kind, the reason)
  void noteInbViolation(mlir::Location loc, llvm::StringRef kind,
                        llvm::function_ref<std::string()> reason);
  expr::ExprSet inbSeen;
  std::vector<size_t> assumeSides;
  /// sea.ptr.index (docs/design/PtrIndex.md): with
  /// SeaM a PTR_INDEX node, else the arithmetic of the lowering
  expr::Expr ptrIndexTerm(PtrIndexOp p, OpSemContext &ctx);
  /// memset's value as a cell of \p memTy (see OpSem.cpp)
  expr::Expr cellOfUnit(expr::Expr unit, MemType memTy);
  /// the current op's memory is keyed (see OpSem.cpp)
  bool keyedAccess() const;
  /// the mirror check of a block's keyed accesses (see OpSem.cpp)
  void settleKeyed(OpSemContext &ctx);
  /// this block's program access addresses / ranges, and its keyed
  /// accesses waiting for a mirror
  expr::ExprSet blockAddrs;
  /// range ops: their (address, length) operands; a companion range op has
  /// the data op's own Values (sea-lower-fat-pointers), whatever encoding
  /// (term or store chain) each side gets
  llvm::DenseSet<std::pair<mlir::Value, mlir::Value>> blockRanges;
  std::vector<std::pair<expr::Expr, mlir::Location>> pendingKeyed;
  std::vector<std::tuple<mlir::Value, mlir::Value, mlir::Location>>
      pendingKeyedRanges;
  /// addr.add (see OpSem.cpp)
  expr::Expr addrAdd(expr::Expr a, expr::Expr o);
  /// with SeaM, the lowering's arithmetic of each node, kept alive: its
  /// terms keep the ids the lowered program gave them, so the rewriter's
  /// sum order (by id) of a node's flat form is the old one
  std::vector<expr::Expr> ptrIndexArith;

  expr::ExprFactory &ef;
  ValueNames &names;
  bool nativeMulOverflow;
  bool seamMode;
  bool addressMode = false;
  unsigned numPtrFromInt = 0;
  /// the pointer companion (S6; see ptrCompanion)
  bool ptrCompanionOn = false;
  bool ptrIntAssume = false, ptrIntCheck = false;
  unsigned numCompanionAssumed = 0, numCompanionChecked = 0;
  /// the program has a split pointer load (exec, first call); else no
  /// companion is built
  std::optional<bool> companionNeeded;
  llvm::DenseMap<mlir::Value, expr::Expr> companions;
  unsigned numCompanionLoads = 0, numCompanionStores = 0,
           numCompanionCopies = 0, numCompanionFresh = 0;
  /// M.ptr of memory value \p mem (null: not a pointer-indexed memory)
  expr::Expr companionOf(mlir::Value mem, OpSemContext &ctx);
  /// the pointer a store writes (whole, or the offset-0 byte of a split
  /// pointer store), else null
  expr::Expr storedPointer(StoreOp st, OpSemContext &ctx);
  /// a split pointer load: the companion form of from_bits(bits)
  expr::Expr companionLoad(AddrFromBitsOp op, expr::Expr bits,
                           OpSemContext &ctx);
  bool lambdaMode = false;
  /// width terms for wide accesses of byte memories (SeaMOptions::wideAccess)
  bool wideMode = false;
  /// the `align` annotation of the current load / store (0: none)
  uint64_t curAlign() const;
  /// a wide load / store of a byte memory as a width term (\p bits > 8)
  expr::Expr memLoadWide(expr::Expr mem, expr::Expr ptr, unsigned bits,
                         OpSemContext &ctx);
  expr::Expr memStoreWide(expr::Expr mem, expr::Expr ptr, expr::Expr val,
                          unsigned bits, OpSemContext &ctx);
  bool simplifying = false;
  /// lambda encoding: `lambda i. body(i)` over index sort \p idx
  expr::Expr mkLambda(expr::Expr idx,
                      const std::function<expr::Expr(expr::Expr)> &body);
  /// lambda encoding: the index sort of a memory register / value
  expr::Expr indexSortOf(expr::Expr arrayConst);
  expr::Expr trueE, falseE, bv1One, bv1Zero;
  // declared last: destroyed first (its cache holds terms of ef)
  std::unique_ptr<expr::SeaMRewriter> seamRw;
  /// the solver boundary of a VC without the rewriter (S2g): erases the
  /// pointer operators and sort with the lowering's arithmetic
  std::unique_ptr<expr::PtrIndexExpander> plainEraser;
};

} // namespace sea

#endif // SEA_VC_OPSEM_H
