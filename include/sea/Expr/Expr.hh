// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <typeinfo>

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sea/Expr/ExprGmp.hh"

#include "sea/Support/boost_flat_set.hh"
#include <boost/functional/hash_fwd.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/pool/pool.hpp>
#include <boost/pool/poolfwd.hpp>

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/Casting.h"

#define mk_it_range llvm::make_range

#include "sea/Expr/ExprCore.hh"
#include "sea/Expr/ExprOpCore.hh"

#include "sea/Expr/ExprApi.hh"

#include "sea/Expr/ExprOpBool.hh"

#include "sea/Expr/ExprOpArray.hh"
#include "sea/Expr/ExprOpBind.hh"
#include "sea/Expr/ExprOpCompare.hh"
#include "sea/Expr/ExprOpGate.hh"
#include "sea/Expr/ExprOpNum.hh"
#include "sea/Expr/ExprOpSort.hh"
#include "sea/Expr/ExprOpStruct.hh"
#include "sea/Expr/ExprOpVariant.hh"

#include "sea/Expr/ExprOpBv.hh"
#include "sea/Expr/ExprOpPtr.hh"

#include "sea/Expr/ExprOpMisc.hh"

namespace expr {
namespace op {}
/** Size of an expression as a DAG */
size_t dagSize(Expr e);
size_t dagSize(const ExprVector &vec);
/** A term as a log value: printed when its DAG has at most 64 nodes, else
    `#<id> (<n> nodes)` (sea/Support/Log.h) */
std::string termForLog(Expr e);
/** Size of an expression as a tree */
size_t treeSize(Expr e);

// -- replace all occurrences of s by t
Expr replaceAll(Expr exp, Expr s, Expr t);
/** Replace all occurrences of s by t while simplifying the result */
Expr replaceAllSimplify(Expr exp, Expr s, Expr t);
/** Returns true if e1 contains e2 as a sub-expression */
bool contains(Expr e1, Expr e2);
} // namespace expr
