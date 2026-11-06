// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

/// Expr terminals for MLIR values and blocks (the sea-mlir counterpart of
/// SeaHorn's ExprLlvm.hh).
///
/// A terminal holds the IR object, which is its identity (equality and
/// hashing), and a name, which is how it prints and therefore the name of the
/// solver symbol built from it. Names come from ValueNames below.
#pragma once

#include "sea/Expr/Expr.hh"

#include "mlir/IR/Block.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Value.h"

#include <boost/functional/hash.hpp>

namespace sea {
/// An mlir::Value with the name it prints as.
struct ValueRef {
  mlir::Value value;
  mlir::StringAttr name;
};

/// An mlir::Block with the name it prints as.
struct BlockRef {
  mlir::Block *block;
  mlir::StringAttr name;
};
} // namespace sea

namespace expr {
template <>
struct TerminalTrait<sea::ValueRef> {
  static inline void print(std::ostream &OS, const sea::ValueRef &v, int depth,
                           bool brkt) {
    OS << v.name.getValue().str();
  }
  static inline bool less(const sea::ValueRef &v1, const sea::ValueRef &v2) {
    return v1.value.getAsOpaquePointer() < v2.value.getAsOpaquePointer();
  }
  static inline bool equal_to(const sea::ValueRef &v1,
                              const sea::ValueRef &v2) {
    return v1.value == v2.value;
  }
  static inline size_t hash(const sea::ValueRef &v) {
    boost::hash<const void *> hasher;
    return hasher(v.value.getAsOpaquePointer());
  }
  static TerminalKind getKind() { return TerminalKind::MLIR_VALUE; }
  static std::string name() { return "mlir::Value"; }
  static inline Expr inferType(Expr exp, TypeChecker &tc) {
    return op::sort::mlirValueTerminalTy(exp->efac());
  }
};

template <>
struct TerminalTrait<sea::BlockRef> {
  static inline void print(std::ostream &OS, const sea::BlockRef &b, int depth,
                           bool brkt) {
    OS << b.name.getValue().str();
  }
  static inline bool less(const sea::BlockRef &b1, const sea::BlockRef &b2) {
    return b1.block < b2.block;
  }
  static inline bool equal_to(const sea::BlockRef &b1,
                              const sea::BlockRef &b2) {
    return b1.block == b2.block;
  }
  static inline size_t hash(const sea::BlockRef &b) {
    boost::hash<const void *> hasher;
    return hasher(b.block);
  }
  static TerminalKind getKind() { return TerminalKind::MLIR_BLOCK; }
  static std::string name() { return "mlir::Block"; }
  static inline Expr inferType(Expr exp, TypeChecker &tc) {
    return op::sort::mlirBlockTerminalTy(exp->efac());
  }
};

namespace op {
using MLIR_VALUE = Terminal<sea::ValueRef>;
using MLIR_BLOCK = Terminal<sea::BlockRef>;
} // namespace op
} // namespace expr
