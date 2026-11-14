// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/// Core of the Expr library
#pragma once

#include "sea/Support/boost_ptr_vector.hh"
#include <boost/functional/hash_fwd.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/pool/pool.hpp>

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"

#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <type_traits>
#include <unordered_set>
#include <vector>
namespace expr {

/// Counter invariants of the Expr core (always checked, Release too: the
/// large optimized runs are where they would be broken). A violation stops
/// the process at its cause, before the counter changes: no recovery, no
/// saturation. \p kind is one of expr.refcount-overflow,
/// expr.refcount-underflow, expr.id-exhausted. Prints the kind, the factory
/// and node pointers, the counter and its limit to stderr (no allocation, no
/// printing of the expression: the node may be half-built or recycled) and
/// aborts. Cold and out of line (ExprCore.cpp): a stable breakpoint.
[[noreturn]] void fatalCounter(const char *kind, const void *factory,
                               const void *node, unsigned long long value,
                               unsigned long long limit);

struct ExprCoreTestAccess; // unittests/ExprCore: sets counters near limits

namespace op {}
using namespace expr::op;

class ENode;
class ExprFactory;
class ExprFactoryAllocator;
class TypeChecker;

using Expr = boost::intrusive_ptr<ENode>;
using ExprSet = std::set<Expr>;
using ExprVector = std::vector<Expr>;
using ExprPair = std::pair<Expr, Expr>;
using ExprMap = std::map<Expr, Expr>;

/* Helper functions to convert from different wrappers over
   expressions into a pointer to an expression node */
inline ENode *eptr(ENode *p) { return p; }
inline const ENode *eptr(const ENode *p) { return p; }
inline ENode *eptr(ENode &p) { return &p; }
inline const ENode *eptr(const ENode &p) { return &p; }
inline ENode *eptr(const Expr &e) { return e.get(); }
inline ENode *eptr(Expr &e) { return e.get(); }

enum class OpFamilyId {
  Terminal,
  BoolOp,
  CompareOp,
  NumericOp,
  MiscOp,
  SimpleTypeOp,
  TerminalTypeOp,
  ArrayOp,
  StructOp,
  FiniteMapOp,
  VariantOp,
  BindOp,
  BinderOp,
  BvOp,
  GateOp,
  MutModelOp,
  PtrOp,
};

/// \brief An operator labeling a node of an expression tree
class Operator {
  /// \brief Family to which the operator belongs
  OpFamilyId m_familyId;

public:
  /// \brief No default constructor
  Operator() = delete;
  Operator(OpFamilyId family) : m_familyId(family) {}
  virtual ~Operator() {}

  /// \brief Return family of the operator
  OpFamilyId getFamilyId() const { return m_familyId; }

  /** Print an expression rooted at the operator
      OS    -- the output strream
      args  -- the arguments of the operator
      depth -- indentation level for any new line
      brkt  -- whether the context in which the operator is printed
            -- might be ambiguous and brackets might be required
   **/
  virtual void Print(std::ostream &OS, const std::vector<ENode *> &args,
                     int depth = 0, bool brkt = true) const = 0;
  /// Equality of operators; false for operators of different classes (one
  /// unique table holds the nodes of all operators, ExprFactory). Terminals
  /// rely on their TerminalKind for this: one kind per terminal type.
  virtual bool operator==(const Operator &rhs) const = 0;
  virtual bool operator<(const Operator &rhs) const = 0;
  virtual size_t hash() const = 0;
  virtual bool isMutable() const { return false; }
  /// \brief Returns heap-allocated copy of the operator
  virtual Operator *clone(ExprFactoryAllocator &allocator) const = 0;
  virtual std::string name() const = 0;
  /// \return true for the typechecker to infer the type of the current
  /// expression before visiting its children
  virtual bool typeCheckTopDown() const = 0;
  /// \brief Returns the type of the expression
  virtual Expr inferType(Expr exp, TypeChecker &tc) const = 0;
};

inline std::ostream &operator<<(std::ostream &OS, const Operator &V) {
  std::vector<ENode *> x;
  V.Print(OS, x);
  return OS;
}

struct EFADeleter {
  ExprFactoryAllocator *m_efa;
  EFADeleter(ExprFactoryAllocator &efa) : m_efa(&efa) {}
  EFADeleter(const EFADeleter &) = default;
  EFADeleter &operator=(const EFADeleter &o) = default;
  /// Destroys the operator (its value: a string, a GMP number) and frees its
  /// storage in the factory's pools.
  void operator()(Operator *p);
};

/// \brief An expression tree node
class ENode {
protected:
  /** unique identifier of this expression node */
  unsigned int id;
  /** reference counter */
  unsigned int count;

  /// \brief Parent factory that created this node
  ExprFactory *fac;

  /// \brief Pointer to children / arguments of this node
  std::vector<ENode *> args;

  /// \brief Operator labeling the node
  std::unique_ptr<Operator, EFADeleter> m_oper;

  /// Release one reference. Releasing a node whose count is 0 is a bug
  /// (expr.refcount-underflow): a published node in its adoption interval
  /// (count 0 until the first Expr wraps it) has no reference to release.
  void Deref() {
    if (count == 0)
      fatalCounter("expr.refcount-underflow", fac, this, count, 0);
    --count;
  }

  /// \brief Set id of the node
  void setId(unsigned int v) { id = v; }

public:
  ENode(ExprFactory &f, const Operator &o);
  ~ENode();

  ENode() = delete;
  ENode(const ENode &) = delete;

  ExprFactory &getFactory() const { return *fac; }
  ExprFactory &efac() const { return getFactory(); }

  /** returns the unique id of this expression */
  unsigned int getId() const { return id; }

  LLVM_ATTRIBUTE_ALWAYS_INLINE void Ref() {
    if (LLVM_UNLIKELY(count == std::numeric_limits<unsigned int>::max()))
      fatalCounter("expr.refcount-overflow", fac, this, count, count);
    ++count;
  }
  bool isGarbage() const { return count == 0; }
  /// Release a reference that is not the last one: true when done (count was
  /// > 1); false leaves the count unchanged for ExprFactory::derefLast (the
  /// last reference, and an underflow).
  LLVM_ATTRIBUTE_ALWAYS_INLINE bool releaseShared() {
    if (count > 1) {
      --count;
      return true;
    }
    return false;
  }
  bool isMutable() const { return m_oper->isMutable(); }

  unsigned int use_count() { return count; }

  ENode *operator[](size_t p) { return arg(p); }
  ENode *arg(size_t p) { return args[p]; }

  ENode *left() { return (args.size() > 0) ? args[0] : nullptr; }

  ENode *right() { return (args.size() > 1) ? args[1] : nullptr; }

  ENode *first() { return left(); }
  ENode *last() { return args.size() > 0 ? args[args.size() - 1] : nullptr; }

  using args_const_iterator = std::vector<ENode *>::const_iterator;

  bool args_empty() const { return args.empty(); }
  args_const_iterator args_begin() const { return args.begin(); }
  args_const_iterator args_end() const { return args.end(); }

  args_const_iterator begin() const { return args_begin(); }
  args_const_iterator end() const { return args_end(); }

  template <typename iterator>
  void renew_args(iterator b, iterator e);

  void push_back(ENode *a) {
    args.push_back(a);
    a->Ref();
  }

  size_t arity() const { return args.size(); }

  const Operator &op() const { return *m_oper; }
  void Print(std::ostream &OS, int depth = 0, bool brkt = true) const {
    m_oper->Print(OS, args, depth, brkt);
  }
  void dump() const {
    Print(std::cerr, 0, false);
    std::cerr << std::endl;
  }

  friend class ExprFactory;
  friend struct std::less<expr::ENode *>;
  friend struct ExprCoreTestAccess;
};

inline std::ostream &operator<<(std::ostream &OS, const ENode &V) {
  V.Print(OS);
  return OS;
}
inline std::ostream &operator<<(std::ostream &OS, const ENode *v) {
  if (v == nullptr)
    OS << "nullptr";
  else
    OS << *v;
  return OS;
}

/// The hash of the unique table: operator \p op applied to the arguments
/// [b, e). One definition for the nodes in the table and for lookup keys.
template <typename iterator>
size_t uniqueHash(const Operator &op, iterator b, iterator e) {
  size_t res = op.hash();
  if (b == e)
    return res;
  boost::hash_combine(res, *b);
  if (std::next(b) != e)
    boost::hash_combine(res, boost::hash_range(b, e));
  return res;
}

/// The structure the unique table compares: an operator applied to the
/// argument pointers [b, e) (a node, or a candidate no node holds yet).
struct UniqueProbe {
  const Operator *op;
  ENode *const *b;
  ENode *const *e;
  size_t hash;
};

/// An element of the unique table (\p node set), or a lookup key that
/// borrows a candidate's operator and arguments for one lookup (\p probe
/// set): ExprFactory looks a candidate up before it builds a node.
struct UniqueKey {
  ENode *node = nullptr;
  const UniqueProbe *probe = nullptr;
};

struct UniqueKeyHash {
  size_t operator()(const UniqueKey &k) const {
    if (k.probe)
      return k.probe->hash;
    return uniqueHash(k.node->op(), k.node->args_begin(), k.node->args_end());
  }
};

/// Same arity, same operator, identical children.
struct UniqueKeyEqual {
  bool operator()(const UniqueKey &x, const UniqueKey &y) const {
    if (x.probe)
      return y.probe ? equal(*x.probe, *y.probe) : equal(*x.probe, y.node);
    return y.probe ? equal(*y.probe, x.node) : equal(x.node, y.node);
  }

private:
  static bool equal(const UniqueProbe &p, const ENode *n) {
    return size_t(p.e - p.b) == n->arity() && *p.op == n->op() &&
           std::equal(p.b, p.e, n->args_begin());
  }
  static bool equal(const UniqueProbe &p, const UniqueProbe &q) {
    return p.e - p.b == q.e - q.b && *p.op == *q.op &&
           std::equal(p.b, p.e, q.b);
  }
  static bool equal(const ENode *n, const ENode *m) {
    return n->arity() == m->arity() && n->op() == m->op() &&
           std::equal(n->args_begin(), n->args_end(), m->args_begin());
  }
};

/// \brief Type erasure for Cache
struct CacheStub {
  /// brief Returns true if the stub own the cache pointer by pointer \p p
  virtual bool owns(const void *p) = 0;
  /// \brief Removes a given value \p val from the cache
  virtual void erase(ENode *val) = 0;

  CacheStub() = default;
  virtual ~CacheStub() = default;
};

template <typename C>
struct CacheStubImpl : CacheStub {

  C &m_cache;

  CacheStubImpl(C &c) : m_cache(c) {}

  virtual bool owns(const void *p) {
    return p == static_cast<const void *>(&m_cache);
  }
  virtual void erase(ENode *val) { m_cache.erase(val); }
};

class ExprFactoryAllocator {
private:
  /** pool for tiny objects */
  boost::pool<> tiny;
  /** pool for small objects */
  boost::pool<> small;

public:
  ExprFactoryAllocator() : tiny(8, 65536), small(64, 65536) {}
  ExprFactoryAllocator(const ExprFactoryAllocator &) = delete;

  void *allocate(size_t n);
  void free(void *block);

  EFADeleter get_deleter();
};

class ExprFactory : boost::noncopyable {
protected:
  // -- type of the unique table: one set for all operators (the hash and
  // -- the equality include the operator, Operator::operator==)
  using unique_type =
      std::unordered_set<UniqueKey, UniqueKeyHash, UniqueKeyEqual>;

  using caches_type = boost::ptr_vector<CacheStub>;

  /** pool allocator */
  ExprFactoryAllocator allocator;

  /** list of registered caches */
  caches_type caches;

  // -- unique table
  unique_type unique;

  /** counter for assigning unique ids*/
  unsigned int idCount;

  /** returns a unique id > 0 (ids are factory-local, never reused: 0 means
      unassigned; exhausting them is expr.id-exhausted) */
  unsigned int uniqueId() {
    if (idCount == std::numeric_limits<unsigned int>::max())
      fatalCounter("expr.id-exhausted", this, nullptr, idCount, idCount);
    return ++idCount;
  }

  /**
   * Remove value from unique table
   */
  void Remove(ENode *val) {
    clearCaches(val);
    if (!val->isMutable()) {
      // -- can only remove things that have been inserted before
      [[maybe_unused]] size_t n = unique.erase(UniqueKey{val});
      assert(n == 1);
    }
    freeNode(val);
  }

  /**
   * Clear val from all registered caches
   */
  void clearCaches(ENode *val) {
    for (CacheStub &c : caches)
      c.erase(val);
  }

  /// The node \p op applied to the arguments [b, e): the node of the unique
  /// table if there is one (a lookup: no operator clone, no allocation, no
  /// reference counting), else a new node (the operator cloned, a reference
  /// to every argument, the next id). Nodes of mutable operators are never
  /// shared: always new, never in the table.
  ENode *mkNode(const Operator &op, ENode *const *b, ENode *const *e) {
    const bool shared = !op.isMutable();
    if (shared) {
      UniqueProbe p{&op, b, e, uniqueHash(op, b, e)};
      auto it = unique.find(UniqueKey{nullptr, &p});
      if (it != unique.end())
        return it->node;
    }
    ENode *v = allocNode(op);
    for (; b != e; ++b)
      v->push_back(*b);
    if (shared) {
      [[maybe_unused]] bool inserted = unique.insert(UniqueKey{v}).second;
      assert(inserted);
    }
    v->setId(uniqueId());
    return v;
  }

  ENode *mkExpr(const Operator &op) { return mkNode(op, nullptr, nullptr); }

  template <typename etype>
  ENode *mkExpr(const Operator &op, etype e) {
    ENode *a[] = {eptr(e)};
    return mkNode(op, std::begin(a), std::end(a));
  }

  /** binary */
  template <typename etype>
  ENode *mkExpr(const Operator &op, etype e1, etype e2) {
    ENode *a[] = {eptr(e1), eptr(e2)};
    return mkNode(op, std::begin(a), std::end(a));
  }

  /** ternary */
  template <typename etype>
  ENode *mkExpr(const Operator &op, etype e1, etype e2, etype e3) {
    ENode *a[] = {eptr(e1), eptr(e2), eptr(e3)};
    return mkNode(op, std::begin(a), std::end(a));
  }

  /** n-ary
     iterator ranges over ENode* or Expr. Each element is dereferenced once
     here; callers may dereference or traverse the range again. An iterator that
     yields values (e.g. an Expr made on the fly) has them held here until the
     node holds its own references.
  */
  template <typename iterator>
  ENode *mkNExpr(const Operator &op, iterator begin, iterator end) {
    if constexpr (std::is_lvalue_reference_v<decltype(*begin)>) {
      // the range owns its elements: plain pointers suffice
      llvm::SmallVector<ENode *, 8> a;
      for (; begin != end; ++begin)
        a.push_back(eptr(*begin));
      return mkNode(op, a.begin(), a.end());
    } else {
      llvm::SmallVector<Expr, 8> held;
      llvm::SmallVector<ENode *, 8> a;
      for (; begin != end; ++begin) {
        held.push_back(Expr(eptr(*begin)));
        a.push_back(held.back().get());
      }
      return mkNode(op, a.begin(), a.end());
    }
  }

private:
#define FREE_LIST_MAX_SIZE 1024 * 4
  std::vector<ENode *> freeList;
  /// nodes whose count reached 0, to remove (Deref)
  std::vector<ENode *> dying;
  bool releasing = false;
  void freeNode(ENode *n);
  ENode *allocNode(const Operator &op);

public:
  ExprFactory() : idCount(0) {}
  /// The nodes made so far, freed ones included (the ids handed out): a
  /// constant-time measure of the terms built, for flow logs.
  unsigned nodesMade() const { return idCount; }
  /// Destroys the recycled nodes kept on the free list (their argument
  /// vectors); every Expr of this factory must be gone before.
  ~ExprFactory();

  /** Release one reference of \p val; a node whose count reaches 0 is
      removed and recycled, and the references it held are released in turn.
      Iterative: releasing the root of a deep chain (a VC store chain) does
      not recurse once per level; nodes that die during a release are queued
      and removed by the outermost call. A reference that is not the last one
      is released inline (the common case); the rest goes out of line. */
  LLVM_ATTRIBUTE_ALWAYS_INLINE void Deref(ENode *val) {
    if (LLVM_LIKELY(val->releaseShared()))
      return;
    derefLast(val);
  }

  /// The slow path of Deref: the last reference (or an underflow, which
  /// aborts in ENode::Deref).
  [[gnu::cold]] LLVM_ATTRIBUTE_NOINLINE void derefLast(ENode *val) {
    val->Deref();
    if (!val->isGarbage())
      return;
    dying.push_back(val);
    if (releasing)
      return;
    releasing = true;
    while (!dying.empty()) {
      ENode *n = dying.back();
      dying.pop_back();
      Remove(n);
    }
    releasing = false;
  }

  /*===================== PUBLIC API ========================================*/

  Expr mkTerm(const Operator &o) { return Expr(mkExpr(o)); }
  Expr mkUnary(const Operator &o, const Expr &e) {
    return Expr(mkExpr(o, e.get()));
  }
  Expr mkBin(const Operator &o, const Expr &e1, const Expr &e2) {
    return Expr(mkExpr(o, e1.get(), e2.get()));
  }
  Expr mkTern(const Operator &o, const Expr &e1, const Expr &e2,
              const Expr &e3) {
    return Expr(mkExpr(o, e1.get(), e2.get(), e3.get()));
  }
  template <typename iterator>
  Expr mkNary(const Operator &o, iterator b, iterator e) {
    return Expr(mkNExpr(o, b, e));
  }

  template <typename Range>
  Expr mkNary(const Operator &o, const Range &r) {
    return mkNary(o, begin(r), end(r));
  }

  /// Registered caches (no callers in sea-mlir today). Contract: unregister
  /// a cache before destroying it; do not register or unregister during
  /// invalidation; an erase callback must not intern terms, resurrect the
  /// dying node or mutate the same cache reentrantly (Remove clears caches
  /// before unlinking the node from the unique table).
  template <typename Cache>
  void registerCache(Cache &cache) {
    // -- to avoid double registration
    unregisterCache(cache);
    caches.push_back(static_cast<CacheStub *>(new CacheStubImpl<Cache>(cache)));
  }

  template <typename Cache>
  bool unregisterCache(const Cache &cache) {
    const void *ptr = static_cast<const void *>(&cache);

    for (caches_type::iterator it = caches.begin(), end = caches.end();
         it != end; ++it)
      if (it->owns(ptr)) {
        caches.erase(it);
        return true;
      }
    return false;
  }

  friend class ENode;
  friend struct ExprCoreTestAccess;
};

inline ENode::ENode(ExprFactory &f, const Operator &o)
    : id(0), count(0), fac(&f),
      m_oper(o.clone(f.allocator), f.allocator.get_deleter()) {}

} // namespace expr

inline void *operator new(size_t n, expr::ExprFactoryAllocator &alloc) {
  return alloc.allocate(n);
}

inline void operator delete(void *p, expr::ExprFactoryAllocator &alloc) {
  alloc.free(p);
}

namespace expr {

/// Disposes of a dead node (removed from the unique table): releases the
/// references it holds and destroys its operator; then keeps the storage on
/// the free list (with its argument vector's capacity) or, with a full list,
/// destroys the node and frees it.
inline void ExprFactory::freeNode(ENode *n) {
  assert(n->count == 0);
  for (ENode *a : n->args)
    Deref(a);
  n->args.clear();
  n->m_oper.reset();
  n->id = 0; // unassigned until mkNode gives it a new one
  if (freeList.size() < FREE_LIST_MAX_SIZE) {
    freeList.push_back(n);
    return;
  }
  n->~ENode(); // no arguments left: frees the vector storage only
  operator delete(static_cast<void *>(n), allocator);
}

inline ExprFactory::~ExprFactory() {
  for (ENode *n : freeList) {
    n->~ENode();
    operator delete(static_cast<void *>(n), allocator);
  }
}

inline ENode *ExprFactory::allocNode(const Operator &op) {
  if (freeList.empty())
    return new (allocator) ENode(*this, op);

  ENode *res = freeList.back();
  freeList.pop_back();
  res->m_oper = std::unique_ptr<Operator, EFADeleter>(op.clone(allocator),
                                                      allocator.get_deleter());
  assert(res->count == 0);
  return res;
}

inline void *ExprFactoryAllocator::allocate(size_t n) {
  if (n <= tiny.get_requested_size())
    return tiny.malloc();
  else if (n <= small.get_requested_size())
    return small.malloc();

  return static_cast<void *>(new char[n]);
}

inline void ExprFactoryAllocator::free(void *block) {
  if (tiny.is_from(block))
    tiny.free(block);
  else if (small.is_from(block))
    small.free(block);
  else
    delete[] static_cast<char *const>(block);
}

inline EFADeleter ExprFactoryAllocator::get_deleter() {
  return EFADeleter(*this);
}

inline void EFADeleter::operator()(Operator *p) {
  p->~Operator();
  operator delete(static_cast<void *>(p), *m_efa);
}

template <typename iterator>
void ENode::renew_args(iterator b, iterator e) {
  std::vector<ENode *> old = args;
  args = std::vector<ENode *>();

  // -- increment reference count of all new arguments
  for (; b != e; ++b)
    this->push_back(eptr(*b));

  // -- decrement reference count of all old arguments
  for (auto b = old.begin(), e = old.end(); b != e; ++b)
    efac().Deref(*b);
}

inline ENode::~ENode() {
  for (auto b = args.begin(), e = args.end(); b != e; ++b)
    efac().Deref(*b);
}

/** Required by boost::intrusive_ptr */
LLVM_ATTRIBUTE_ALWAYS_INLINE void intrusive_ptr_add_ref(ENode *v) { v->Ref(); }

LLVM_ATTRIBUTE_ALWAYS_INLINE void intrusive_ptr_release(ENode *v) {
  // fast path: a reference that is not the last one is released inline; the
  // last one (and an underflow) goes through the factory
  if (LLVM_LIKELY(v->releaseShared()))
    return;
  v->efac().derefLast(v);
}

} // namespace expr

// ========================== HASHING ======================================
namespace expr {
inline size_t hash_value(Expr e) {
  if (!e)
    return 0;
  std::hash<unsigned int> hasher;
  return hasher(e->getId());
}
} // namespace expr

/// implement boost::hash
namespace boost {
template <>
struct hash<expr::Expr> {
  std::size_t operator()(const expr::Expr &v) const {
    return expr::hash_value(v);
  }
};
} // namespace boost

/// implement std::hash<expr::Expr>
namespace std {
template <>
struct hash<expr::Expr> {
  std::size_t operator()(const expr::Expr &v) const {
    return expr::hash_value(v);
  }
};
} // namespace std

/// std::less<expr::ENode*>
namespace std {
/** standard order of expressions by their id */
template <>
struct less<expr::ENode *> {
  bool operator()(const expr::ENode *x, const expr::ENode *y) const {
    if (x == nullptr)
      return y != nullptr;
    if (y == nullptr)
      return false;

    return x->getId() < y->getId();
  }
};
} // namespace std
