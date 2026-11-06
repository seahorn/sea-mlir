// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once
// The base registry (SeaM, docs/design/SeaM.md "The VC keeps bases
// symbolic"): the object base constants of a VC (`obj.<name>`), each with
// what the rewriter may need to know about its object. The VC generator
// (SeaOpSem, with SeaM on) fills it from the sea.segment ops; the rewriter
// reads it and never parses names. A term is a pointer into object k when
// it normalizes to bvadd(obj.k, off) with obj.k registered here.
#include "sea/Expr/Expr.hh"

#include "llvm/ADT/APInt.h"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace expr {

/// One registered object.
struct ObjectBase {
  /// The bit-vector constant standing for the object's base address.
  Expr constant;
  /// Position in allocation order (0 = first): the order of the sea.segment
  /// ops, which is the order of the allocation sites in the program text.
  unsigned order;
  /// The segment's name, `<kind>.<site or name>`; for messages only.
  std::string name;
  /// stack, heap, global or func.
  std::string kind;
  /// Placement: the concrete base, the size in address units, the alignment.
  llvm::APInt base, size;
  uint64_t align;
};

class BaseRegistry {
public:
  /// Registers `constant` (idempotent: a second add returns the entry made
  /// by the first). The order is the number of objects registered before.
  const ObjectBase &add(Expr constant, std::string name, std::string kind,
                        const llvm::APInt &base, const llvm::APInt &size,
                        uint64_t align) {
    auto it = m_index.find(constant);
    if (it != m_index.end())
      return m_objects[it->second];
    if (!size.isZero())
      m_ranges.emplace(base.zext(kRangeWidth), m_objects.size());
    m_index.emplace(constant, m_objects.size());
    m_objects.push_back({constant, (unsigned)m_objects.size(), std::move(name),
                         std::move(kind), base, size, align});
    return m_objects.back();
  }

  /// An already registered object of nonzero size whose address range
  /// [base, base + size) overlaps the range of a new object (null if none, or
  /// if the new object has size 0: it has no addresses). Ranges are those of
  /// the placement; A-INB needs them disjoint (the compare of two pointers
  /// into different objects is false only if the objects share no address).
  const ObjectBase *overlapping(const llvm::APInt &base,
                                const llvm::APInt &size) const {
    if (size.isZero())
      return nullptr;
    llvm::APInt b = base.zext(kRangeWidth), e = b + size.zext(kRangeWidth);
    auto next = m_ranges.lower_bound(b); // first range starting at or after b
    if (next != m_ranges.end() && next->first.ult(e))
      return &m_objects[next->second];
    if (next != m_ranges.begin()) {
      auto prev = std::prev(next);
      const ObjectBase &o = m_objects[prev->second];
      if ((prev->first + o.size.zext(kRangeWidth)).ugt(b))
        return &o;
    }
    return nullptr;
  }

  /// \p twin names the object of base constant \p constant too: the
  /// erased bv(w) constant of a ptr(w) base (PtrSort S2e), so the readers of
  /// the solver's terms (eq-audit's shapes, xrt) know the object.
  void addAlias(const Expr &twin, const Expr &constant) {
    auto it = m_index.find(constant);
    if (it != m_index.end() && twin != constant)
      m_index.emplace(twin, it->second);
  }

  /// The object whose base constant is `e` (or its erased twin), or null.
  const ObjectBase *lookup(const Expr &e) const {
    auto it = m_index.find(e);
    return it == m_index.end() ? nullptr : &m_objects[it->second];
  }
  bool isBase(const Expr &e) const { return lookup(e) != nullptr; }

  /// Shadow memories (docs/design/SeaM-null.md, "Shadow memories"): the
  /// start term of a memory a tool made for metadata keyed by object
  /// identity (`sea.mem.const ... shadow`). Every memory term built over one
  /// (stores, store-maps, ites) is a shadow memory: its accesses are not
  /// program accesses, so no A-INB rule applies to them
  /// (seam::Rules::shadowMemory).
  void addShadowRoot(const Expr &root) { m_shadowRoots.insert(root); }
  bool isShadowRoot(const Expr &e) const { return m_shadowRoots.count(e); }
  bool hasShadowRoots() const { return !m_shadowRoots.empty(); }

  /// In allocation order. References are invalidated by add.
  const std::vector<ObjectBase> &objects() const { return m_objects; }
  size_t size() const { return m_objects.size(); }
  bool empty() const { return m_objects.empty(); }

private:
  /// Addresses and sizes are compared at one width, wide enough for any
  /// base + size.
  static constexpr unsigned kRangeWidth = 130;
  struct UltLess {
    bool operator()(const llvm::APInt &a, const llvm::APInt &b) const {
      return a.ult(b);
    }
  };
  std::vector<ObjectBase> m_objects;
  /// Start of each nonzero-size object -> its index.
  std::map<llvm::APInt, size_t, UltLess> m_ranges;
  std::unordered_map<Expr, size_t> m_index;
  std::unordered_set<Expr> m_shadowRoots;
};

} // namespace expr
