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
  /// the placement; A-INB implies A-SEP only if they are disjoint (two
  /// in-bounds accesses through different objects share no cell only if the
  /// objects share no address).
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
  /// the solver's terms (eq-audit's shapes) know the object.
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
  /// program accesses, so no premise rule (A-SEP, A-NULL) applies to them
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

namespace seam {
/// Where a static out-of-object access lands (seam::Rules::
/// outOfObjectSpans, classified pairwise by SeaOpSem), worst first:
///   Object   a span meets another placed object: A-SEP fails if the
///            access executes;
///   Shared   a span lands in a gap cell that an out-of-object access
///            through another provenance also reaches (one of the two a
///            write): A-SEP fails if both execute;
///   Unknown  a span is not known (a symbolic offset or length, an ARM-only
///            leaf), or a gap span another such access may reach;
///   Null     through an object of size 0: A-NULL fails if it executes;
///   Gap      every span lands in cells that no other flagged access reaches
///            through another provenance (in-bounds accesses never do):
///            A-NULL holds, A-SEP holds as far as the flagged accesses go,
///            only the property A-INB fails.
enum class Landing { Object, Shared, Unknown, Null, Gap };
inline const char *landingName(Landing l) {
  switch (l) {
  case Landing::Object:
    return "object";
  case Landing::Shared:
    return "shared";
  case Landing::Unknown:
    return "unknown";
  case Landing::Null:
    return "null";
  case Landing::Gap:
    return "gap";
  }
  return "?";
}

/// What the placement says about one static out-of-object access, before
/// the pairwise step: per address leaf, whether its span meets another
/// object, is unknown, or goes through an object of size 0, and the spans
/// that land outside every object (gap spans, with their provenance).
struct OutOfObjectSpans {
  bool object = false, unknown = false, null = false;
  /// some leaf's address wraps: its placed base plus its numeral offset,
  /// read as an integer with the offset signed, is outside [0, 2^w) (the
  /// access breaks A-NOWRAP if it executes through that leaf)
  bool wrap = false;
  /// [lo, hi) as integers, lo in [0, 2^w) (hi may pass 2^w); loDown /
  /// hiDown: the same span one address space down (lo - 2^w, hi - 2^w),
  /// so that a span that wraps is compared at both images.
  struct Span {
    const ObjectBase *base; ///< the provenance
    mpz_class lo, hi, loDown, hiDown;
  };
  std::vector<Span> gaps;

  /// Do two spans share a cell (modulo 2^w)?
  static bool overlap(const Span &a, const Span &b) {
    return (a.lo < b.hi && b.lo < a.hi) ||
           (a.loDown < b.hi && b.lo < a.hiDown) ||
           (b.loDown < a.hi && a.lo < b.hiDown);
  }
};
} // namespace seam

} // namespace expr
