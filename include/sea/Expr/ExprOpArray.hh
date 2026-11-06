// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/// Arrays
#pragma once
#include "sea/Expr/ExprApi.hh"
#include "sea/Expr/ExprCore.hh"
#include "sea/Expr/ExprOpBool.hh"
#include "sea/Expr/ExprOpCore.hh"
#include "sea/Expr/TypeCheckerMapUtils.hh"

namespace expr {

namespace op {
enum class ArrayOpKind {
  SELECT,
  STORE,
  CONST_ARRAY,
  ARRAY_MAP,
  ARRAY_DEFAULT,
  AS_ARRAY,
  STORE_MAP,
  SMAP_CONS,
  SMAP_NIL,
  MEM_COPY,
  MEM_SET,
  MEM_HAVOC,
  W_STORE,
  W_SELECT
};

namespace typeCheck {
namespace arrayType {

static inline bool checkArray(Expr exp, TypeChecker &tc, unsigned numChildren) {
  return exp->arity() == numChildren &&
         correctTypeAny<ARRAY_TY>(exp->first(), tc);
}

static inline void getArrayTypes(Expr exp, TypeChecker &tc, Expr &mapTy,
                                 Expr &indexTy, Expr &valTy) {
  mapTy = tc.typeOf(exp->left());
  indexTy = sort::arrayIndexTy(mapTy);
  valTy = sort::arrayValTy(mapTy);
}

/// ensures that the expression's key type matches the arrays's key type
///  checks for the following children (in order): map, index
///  \return the array's value type
struct Select : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    return typeCheck::mapType::select<ARRAY_TY>(exp, tc, getArrayTypes);
  }
};

/// ensures that the index type and value type match the array's index and value
/// types checks for the following children (in order): array, index, value
/// \return ARRAY_TY
struct Store : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    return typeCheck::mapType::store<ARRAY_TY>(exp, tc, getArrayTypes);
  }
};

/// Expected Children types(in order): TYPE_TY, anything
/// \return: ARRAY_TY
struct Const : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    if (exp->arity() != 2)
      return sort::errorTy(exp->efac());

    Expr domain = exp->left();
    Expr value = exp->right();

    if (isOp<TYPE_TY>(tc.typeOf(domain)))
      return sort::arrayTy(domain, tc.typeOf(value));

    return sort::errorTy(exp->efac());
  }
};

/// Expected Children type: ARRAY_TY
/// \return: ARRAY_TY
struct Default : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {

    if (!checkArray(exp, tc, 1))
      return sort::errorTy(exp->efac());

    Expr array = exp->first();

    return sort::arrayValTy(tc.typeOf(array));
  }
};

/// store-map(arr, base, ocl) (SeaM, thesis Ch. 6, Fig. 6.3): the array
/// `arr` with one cell written for every (offset, value) entry of the
/// offset-value cons list `ocl`, at address base + offset. \return the sort
/// of `arr` (an array). Only the shape of the first two kids and the head
/// of the list are checked: the list is built by op::array::storeMap /
/// smapCons, and walking it here would make every type query linear.
struct StoreMap : public TypeCheckBase {
  /// defined below, after the operators of the list
  inline Expr inferType(Expr exp, TypeChecker &tc) override;
};

/// A memory-operation term (SeaM, docs/design/SeaM-memops.md; MemOps.hh):
/// mcopy(Md, Ms, d, s, n), mset(M, d, n, v), mhavoc(M, H, d, n). \return the
/// sort of the first kid (the memory written), an array. Only the arity and
/// the sort of the memory are checked.
template <unsigned Arity>
struct MemOp : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    if (exp->arity() != Arity || !correctTypeAny<ARRAY_TY>(exp->first(), tc))
      return sort::errorTy(exp->efac());
    return tc.typeOf(exp->first());
  }
};
/// wselect(M, a, S): the sort S (a bit-vector sort of 8r bits) is the
/// result sort; only the arity and the memory are checked.
struct WSelect : public TypeCheckBase {
  inline Expr inferType(Expr exp, TypeChecker &tc) override {
    if (exp->arity() != 4 || !correctTypeAny<ARRAY_TY>(exp->first(), tc))
      return sort::errorTy(exp->efac());
    return exp->arg(2);
  }
};
} // namespace arrayType
} // namespace typeCheck

/// Array operators
NOP_BASE(ArrayOp)

NOP(SELECT, "select", FUNCTIONAL, ArrayOp, typeCheck::arrayType::Select)
NOP(STORE, "store", FUNCTIONAL, ArrayOp, typeCheck::arrayType::Store)
NOP(CONST_ARRAY, "const-array", FUNCTIONAL, ArrayOp,
    typeCheck::arrayType::Const)
NOP(ARRAY_MAP, "array-map", FUNCTIONAL, ArrayOp, typeCheck::Any)
NOP(ARRAY_DEFAULT, "array-default", FUNCTIONAL, ArrayOp,
    typeCheck::arrayType::Default)
NOP(AS_ARRAY, "as-array", FUNCTIONAL, ArrayOp, typeCheck::Any)
/// store-map(arr, base, ocl): see typeCheck::arrayType::StoreMap. Not an
/// SMT-LIB operator: the solvers do not know it, a term that reaches one has
/// to go through expr::expandStoreMaps (include/sea/Expr/StoreMap.hh) first.
NOP(STORE_MAP, "store-map", FUNCTIONAL, ArrayOp, typeCheck::arrayType::StoreMap)
/// The offset-value cons list of a store-map (thesis Fig. 7.2):
/// smap-cons(offset, value, rest), ending in smap-nil. Insertion ordered:
/// the head is the latest insertion; an older entry for the same offset is
/// shadowed by the first one met. Not typed (never a term of its own).
NOP(SMAP_CONS, "smap-cons", FUNCTIONAL, ArrayOp, typeCheck::Any)
NOP(SMAP_NIL, "smap-nil", FUNCTIONAL, ArrayOp, typeCheck::Any)
/// Memory-operation terms (docs/design/SeaM-memops.md, include/sea/Expr/
/// MemOps.hh). Cell j of the result, with in(j, d, n) := (j - d) <u n:
///   mcopy(Md, Ms, d, s, n):  in(j, d, n) ? Ms[s + (j - d)] : Md[j]
///   mset(M, d, n, v):        in(j, d, n) ? v : M[j]
///   mhavoc(M, H, d, n):      in(j, d, n) ? H[j] : M[j]
/// Not SMT-LIB operators: a term that reaches a solver goes through the
/// boundary translation (MemOpTranslator, MemOps.hh) first.
NOP(MEM_COPY, "mcopy", FUNCTIONAL, ArrayOp, typeCheck::arrayType::MemOp<5>)
NOP(MEM_SET, "mset", FUNCTIONAL, ArrayOp, typeCheck::arrayType::MemOp<4>)
NOP(MEM_HAVOC, "mhavoc", FUNCTIONAL, ArrayOp, typeCheck::arrayType::MemOp<4>)
/// Width terms of byte memories (docs/design/SeaM-mixed-width.md):
///   wstore(M, a, v):  the w = bits(v) / 8 bytes of v, little endian, at
///                     a, a + 1, ..., a + w - 1 (bits [8k, 8k + 8) at a + k)
///   wselect(M, a, S): the r bytes at a .. a + r - 1 as one value of sort S
///                     (8r bits), little endian
/// with addresses modulo the index width and 0 < w, r <= 2^n (one access
/// does not revisit an address). Not SMT-LIB operators: the boundary expands
/// them into byte stores / selects (WideExpander, MemOps.hh).
NOP(W_STORE, "wstore", FUNCTIONAL, ArrayOp, typeCheck::arrayType::MemOp<4>)
NOP(W_SELECT, "wselect", FUNCTIONAL, ArrayOp, typeCheck::arrayType::WSelect)

namespace typeCheck {
namespace arrayType {
inline Expr StoreMap::inferType(Expr exp, TypeChecker &tc) {
  if (exp->arity() != 3 || !correctTypeAny<ARRAY_TY>(exp->first(), tc))
    return sort::errorTy(exp->efac());
  Expr ocl = exp->arg(2);
  if (!isOpX<SMAP_CONS>(ocl) && !isOpX<SMAP_NIL>(ocl))
    return sort::errorTy(exp->efac());
  return tc.typeOf(exp->first());
}
} // namespace arrayType
} // namespace typeCheck
} // namespace op

namespace op {
namespace array {
inline Expr select(Expr a, Expr idx) { return mk<SELECT>(a, idx); }
inline Expr store(Expr a, Expr idx, Expr v) { return mk<STORE>(a, idx, v); }
inline Expr constArray(Expr domain, Expr v) {
  return mk<CONST_ARRAY>(domain, v);
}
inline Expr aDefault(Expr a) { return mk<ARRAY_DEFAULT>(a); }

/// The empty offset-value list.
inline Expr smapNil(ExprFactory &efac) { return mk<SMAP_NIL>(efac); }
/// The list `rest` with the entry (offset, value) inserted in front.
inline Expr smapCons(Expr offset, Expr value, Expr rest) {
  return mk<SMAP_CONS>(offset, value, rest);
}
/// store-map(a, base, ocl) (thesis Fig. 6.3 / 7.2).
inline Expr storeMap(Expr a, Expr base, Expr ocl) {
  assert(isOpX<SMAP_CONS>(ocl) || isOpX<SMAP_NIL>(ocl));
  return mk<STORE_MAP>(a, base, ocl);
}
/// mcopy(Md, Ms, d, s, n): Md with [d, d + n) copied from Ms at s (Ms is the
/// source as it was before the copy).
inline Expr memCopy(Expr md, Expr ms, Expr d, Expr s, Expr n) {
  ExprVector args{md, ms, d, s, n};
  return mknary<MEM_COPY>(args.begin(), args.end());
}
/// mset(M, d, n, v): M with [d, d + n) set to the cell value v.
inline Expr memSet(Expr m, Expr d, Expr n, Expr v) {
  ExprVector args{m, d, n, v};
  return mknary<MEM_SET>(args.begin(), args.end());
}
/// mhavoc(M, H, d, n): M with [d, d + n) taken from the array H.
inline Expr memHavoc(Expr m, Expr h, Expr d, Expr n) {
  ExprVector args{m, h, d, n};
  return mknary<MEM_HAVOC>(args.begin(), args.end());
}
/// The promised alignment argument of a width term (SeaM-mixed-width.md
/// rev 5, A-ALIGN): the access's `align` in bytes, 1 if none / unknown.
inline Expr alignArg(ExprFactory &ef, uint64_t align) {
  return mkTerm<unsigned>(unsigned(align ? align : 1), ef);
}
/// wstore(M, a, v, al): the bytes of v at a .. (little endian); al is the
/// access's promised alignment (meaning: not part of the value).
inline Expr wstore(Expr m, Expr a, Expr v, uint64_t align = 1) {
  ExprVector args{m, a, v, alignArg(m->efac(), align)};
  return mknary<W_STORE>(args.begin(), args.end());
}
/// wselect(M, a, S, al): the bytes at a .. as one value of the bit-vector
/// sort S; al the access's promised alignment.
inline Expr wselect(Expr m, Expr a, Expr resultSort, uint64_t align = 1) {
  ExprVector args{m, a, resultSort, alignArg(m->efac(), align)};
  return mknary<W_SELECT>(args.begin(), args.end());
}
/// The promised alignment of a width term (1 for anything else).
inline uint64_t promisedAlign(Expr t) {
  if ((isOpX<W_SELECT>(t) || isOpX<W_STORE>(t)) && t->arity() == 4)
    return getTerm<unsigned>(t->arg(3));
  return 1;
}
} // namespace array
} // namespace op
} // namespace expr
