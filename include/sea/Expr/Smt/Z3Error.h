// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//===- Z3Error.h - z3 errors are reported, never ignored -*- C++ -*-===//
//
// sea-mlir builds without exceptions (like LLVM), so z3++.h cannot throw on a
// z3 error and z3::context turns z3's own handler off: without more, an error
// would go unnoticed. Every z3 context gets a handler (installZ3ErrorHandler)
// that reports diag kind smt.z3-internal and ends the process with a non-zero
// status. It runs only when z3 reports an error. A z3 memory-out is not
// internal: it takes the memory-limit outcome of the application (a hook).
//
// Outside input (a tactic, a --z3-param) is checked where it enters, so the
// user sees the specific kind; a call that checks an error code itself holds a
// Z3ExpectErrors guard, under which the handler leaves the error to the caller.
//
//===----------------------------------------------------------------------===//

#ifndef SEA_EXPR_SMT_Z3ERROR_H
#define SEA_EXPR_SMT_Z3ERROR_H

#include "z3.h"

#include <string>

namespace sea {

/// The application's hooks: \p flushOutput writes its pending output before
/// the process ends on an error; \p memout ends the process with the
/// memory-limit outcome (it does not return)
void setZ3ErrorHooks(void (*flushOutput)(), void (*memout)());

/// Reports \p code of \p ctx: the memory-limit outcome for Z3_MEMOUT_FAIL,
/// else smt.z3-internal and exit(1). Does not return.
[[noreturn]] void z3ReportError(Z3_context ctx, Z3_error_code code);

/// Before z3::context is made from \p cfg: z3++ does not check that z3 made
/// the context, and under a tight memory_max_size it cannot (null). Takes
/// the memory-limit outcome then.
void z3EnsureContext(Z3_config cfg);

/// Makes z3 report an error of \p ctx as smt.z3-internal and exit(1)
void installZ3ErrorHandler(Z3_context ctx);

/// While alive (on this thread), the handler does nothing: the code reads
/// Z3_get_error_code itself
class Z3ExpectErrors {
public:
  Z3ExpectErrors();
  ~Z3ExpectErrors();
  Z3ExpectErrors(const Z3ExpectErrors &) = delete;
  Z3ExpectErrors &operator=(const Z3ExpectErrors &) = delete;
};

/// Z3_global_param_set that reports a rejected name or value: z3 only prints
/// a warning and goes on. The value is read back. False, with the reason in
/// \p err, when rejected.
bool z3SetGlobalParam(const std::string &name, const std::string &value,
                      std::string &err);

/// Whether z3 accepts \p tactic (a name or a tactic expression): it parses
/// and runs it on a trivial query in a context of its own (once per tactic).
/// False, with z3's reason in \p err, when it does not.
bool z3CheckTactic(const std::string &tactic, std::string &err);

/// Test hook (sea-vc --z3-plant-error): an invalid z3 API call on \p ctx
void plantZ3Error(Z3_context ctx);

} // namespace sea

#endif // SEA_EXPR_SMT_Z3ERROR_H
