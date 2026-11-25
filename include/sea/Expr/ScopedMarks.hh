// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/// Scoped marks on Expr nodes: transient node sets without a hash table.
///
/// Every ENode has 64 mark bits (ENode::marks). A scope leases bits from its
/// factory (ExprFactory::leaseMarks: a factory-owned 64-bit occupancy mask)
/// for its lifetime; live scopes hold distinct bits, so nested scopes (a walk
/// inside a walk) do not see each other's marks. Membership is the node's
/// bit; the scope keeps a log of the nodes it marked, holding a reference to
/// each (a marked node stays alive while it is marked), and clears its bits
/// on all of them on clear() and at destruction, before it drops the
/// references; the destructor then returns the bits to the factory.
///
/// Running out of bits (64 live leases on one factory) is a hard failure:
/// expr.marks-exhausted names the live leases and aborts. There is no
/// fallback to a hash set: that would hide a leaked scope or runaway
/// nesting. Scopes are non-copyable and non-movable: a lease belongs to one
/// object.
///
/// Single-threaded, like the rest of the factory (reference counts and the
/// unique table are not synchronized): a factory, its nodes and its scopes
/// are used by one thread.
///
/// Contract: non-null nodes of the scope's factory (a node of another
/// factory is expr.marks-factory, a hard failure: its bits belong to other
/// leases); the factory outlives the scope.
#pragma once

#include "sea/Expr/ExprCore.hh"

#include <cstdint>
#include <vector>

namespace expr {

class ScopedExprSet;
class ScopedUseCount;

/// The node-bit access of the scopes (ENode::marks is private, and so is
/// this access: only the scope classes reach the bits).
struct ExprMarkAccess {
private:
  friend class ScopedExprSet;
  friend class ScopedUseCount;
  static uint64_t &marks(ENode *n) { return n->marks; }
  static uint64_t marks(const ENode *n) { return n->marks; }
  static void checkFactory(const ExprFactory *f, const ENode *n) {
    if (LLVM_UNLIKELY(n->fac != f))
      fatalMarks("expr.marks-factory", f, n->fac);
  }
  /// Clears \p bits on every node of \p log, then drops the references.
  static void clearLog(std::vector<Expr> &log, uint64_t bits) {
    for (const Expr &e : log)
      e->marks &= ~bits;
    log.clear();
  }
};

/// A set of nodes of one factory, as one leased mark bit. insert / contains
/// cost a bit test, insert of a new node a reference and a log entry.
class ScopedExprSet {
public:
  /// Leases one bit of \p f; \p owner (a string literal) names the scope in
  /// expr.marks-exhausted.
  ScopedExprSet(ExprFactory &f, const char *owner)
      : m_fac(&f), m_bit(f.leaseMarks(1, owner)) {}
  ScopedExprSet(const ScopedExprSet &) = delete;
  ScopedExprSet &operator=(const ScopedExprSet &) = delete;
  ScopedExprSet(ScopedExprSet &&) = delete;
  ScopedExprSet &operator=(ScopedExprSet &&) = delete;
  ~ScopedExprSet() {
    clear();
    m_fac->releaseMarks(m_bit);
  }

  /// Adds \p n: true when it was not in the set (the first insertion since
  /// construction or the last clear).
  bool insert(ENode *n) {
    ExprMarkAccess::checkFactory(m_fac, n);
    uint64_t &m = ExprMarkAccess::marks(n);
    if (m & m_bit)
      return false;
    m_log.push_back(Expr(n));
    m |= m_bit;
    return true;
  }
  bool insert(const Expr &e) { return insert(e.get()); }

  bool contains(const ENode *n) const {
    ExprMarkAccess::checkFactory(m_fac, n);
    return ExprMarkAccess::marks(n) & m_bit;
  }
  bool contains(const Expr &e) const { return contains(e.get()); }

  /// Empties the set; keeps the lease.
  void clear() { ExprMarkAccess::clearLog(m_log, m_bit); }

  /// The nodes in the set.
  size_t size() const { return m_log.size(); }
  bool empty() const { return m_log.empty(); }

private:
  ExprFactory *m_fac;
  uint64_t m_bit;
  std::vector<Expr> m_log;
};

/// Use counting up to two, as two leased mark bits: a node is unseen, seen
/// once, or seen more than once ("used more than once" is all a sharing
/// test needs).
class ScopedUseCount {
public:
  ScopedUseCount(ExprFactory &f, const char *owner)
      : m_fac(&f), m_bits(f.leaseMarks(2, owner)), m_once(m_bits & -m_bits),
        m_twice(m_bits & ~m_once) {}
  ScopedUseCount(const ScopedUseCount &) = delete;
  ScopedUseCount &operator=(const ScopedUseCount &) = delete;
  ScopedUseCount(ScopedUseCount &&) = delete;
  ScopedUseCount &operator=(ScopedUseCount &&) = delete;
  ~ScopedUseCount() {
    clear();
    m_fac->releaseMarks(m_bits);
  }

  /// Counts one use of \p n: true on its first use.
  bool add(ENode *n) {
    ExprMarkAccess::checkFactory(m_fac, n);
    uint64_t &m = ExprMarkAccess::marks(n);
    if (m & m_once) {
      m |= m_twice;
      return false;
    }
    m_log.push_back(Expr(n));
    m |= m_once;
    return true;
  }
  bool add(const Expr &e) { return add(e.get()); }

  /// The uses of \p n, saturated at 2.
  unsigned count(const ENode *n) const {
    ExprMarkAccess::checkFactory(m_fac, n);
    uint64_t m = ExprMarkAccess::marks(n);
    return (m & m_twice) ? 2 : (m & m_once) ? 1 : 0;
  }
  unsigned count(const Expr &e) const { return count(e.get()); }
  /// Used more than once.
  bool shared(const ENode *n) const { return count(n) == 2; }
  bool shared(const Expr &e) const { return shared(e.get()); }

  /// Forgets every count; keeps the lease.
  void clear() { ExprMarkAccess::clearLog(m_log, m_bits); }

  /// The nodes with a use.
  size_t size() const { return m_log.size(); }

private:
  ExprFactory *m_fac;
  uint64_t m_bits;
  uint64_t m_once, m_twice;
  std::vector<Expr> m_log;
};

} // namespace expr
