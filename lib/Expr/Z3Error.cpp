// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sea/Expr/Smt/Z3Error.h"

#include "sea/Support/Diag.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <map>
#include <string>
#include <unistd.h>

namespace sea {

namespace {
thread_local unsigned expectDepth = 0;
void (*flushOutputHook)() = nullptr;
void (*memoutHook)() = nullptr;

[[noreturn]] void fatalInternal(Z3_context ctx, Z3_error_code code) {
  const char *msg = Z3_get_error_msg(ctx, code);
  sea::diag::error("smt.z3-internal",
                   llvm::Twine("z3 reported an error (code ") +
                       llvm::Twine(static_cast<int>(code)) +
                       "): " + (msg ? msg : "no message"));
  sea::diag::printSummary();
  if (flushOutputHook)
    flushOutputHook();
  llvm::outs().flush();
  llvm::errs().flush();
  // not exit(): z3 is mid-call, and static destructors would run under it
  _exit(1);
}

void z3ErrorHandler(Z3_context ctx, Z3_error_code code) {
  if (expectDepth)
    return;
  z3ReportError(ctx, code);
}

// z3 prints "(error "line N column M: <reason>")": the reason only
std::string z3ErrorReason(std::string out) {
  size_t at = out.find("(error");
  out.erase(0, at == std::string::npos ? 0 : at);
  while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
    out.pop_back();
  if (out.rfind("(error \"", 0) == 0) {
    out.erase(0, 8);
    if (out.size() >= 2 && out.compare(out.size() - 2, 2, "\")") == 0)
      out.erase(out.size() - 2);
    if (out.rfind("line ", 0) == 0) {
      size_t colon = out.find(": ");
      if (colon != std::string::npos)
        out.erase(0, colon + 2);
    }
  }
  return out;
}

bool sameNumber(const std::string &a, const std::string &b) {
  char *ea = nullptr, *eb = nullptr;
  double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
  return !a.empty() && !b.empty() && !*ea && !*eb && x == y;
}

// no stray "WARNING:" lines from z3 while a parameter is probed
// (the user's --z3-param=warning=false stays in force after it)
bool warningsOn = true;
struct QuietWarnings {
  QuietWarnings() { Z3_toggle_warning_messages(false); }
  ~QuietWarnings() { Z3_toggle_warning_messages(warningsOn); }
};
} // namespace

void z3ReportError(Z3_context ctx, Z3_error_code code) {
  // a memory-out is the memory limit's outcome, not an internal error
  if (code == Z3_MEMOUT_FAIL && memoutHook)
    memoutHook();
  fatalInternal(ctx, code);
}

void setZ3ErrorHooks(void (*flushOutput)(), void (*memout)()) {
  flushOutputHook = flushOutput;
  memoutHook = memout;
}

void installZ3ErrorHandler(Z3_context ctx) {
  Z3_set_error_handler(ctx, z3ErrorHandler);
}

void z3EnsureContext(Z3_config cfg) {
  static bool ok = false;
  if (ok)
    return;
  // z3++ does not check: under a tight memory_max_size no context can be made
  Z3_context probe = Z3_mk_context_rc(cfg);
  if (probe) {
    Z3_del_context(probe);
    ok = true;
    return;
  }
  if (memoutHook)
    memoutHook();
  sea::diag::error("smt.z3-internal", "z3 cannot make a context (memory)");
  sea::diag::printSummary();
  llvm::errs().flush();
  _exit(1);
}

Z3ExpectErrors::Z3ExpectErrors() { ++expectDepth; }
Z3ExpectErrors::~Z3ExpectErrors() { --expectDepth; }

bool z3SetGlobalParam(const std::string &name, const std::string &value,
                      std::string &err) {
  // z3 only warns about an unknown name or a bad value and goes on: set,
  // then read the value back
  QuietWarnings quiet;
  Z3_string old = nullptr;
  if (!Z3_global_param_get(name.c_str(), &old)) {
    err = "unknown parameter '" + name + "'";
    return false;
  }
  Z3_global_param_set(name.c_str(), value.c_str());
  Z3_string now = nullptr;
  if (!Z3_global_param_get(name.c_str(), &now)) {
    err = "unknown parameter '" + name + "'";
    return false;
  }
  std::string held = now ? now : "";
  if (held != value && !sameNumber(held, value)) {
    err = "invalid value '" + value + "' (z3 holds '" + held + "')";
    return false;
  }
  if (name == "warning")
    warningsOn = held == "true";
  return true;
}

bool z3CheckTactic(const std::string &tactic, std::string &err) {
  // once per tactic: every solver made asks
  static std::map<std::string, std::pair<bool, std::string>> cache;
  auto it = cache.find(tactic);
  if (it != cache.end()) {
    err = it->second.second;
    return it->second.first;
  }
  bool ok = true;
  std::string reason;
  Z3_config cfg = Z3_mk_config();
  Z3_context ctx = Z3_mk_context(cfg);
  Z3_del_config(cfg);
  // no context (a memory limit): leave it to the solver made next
  if (ctx) {
    // the answer is read from the output and the error code: the caller
    // reports it
    Z3ExpectErrors expect;
    installZ3ErrorHandler(ctx);
    std::string q =
        "(declare-const x Bool)(assert x)(check-sat-using " + tactic + ")";
    std::string out = Z3_eval_smtlib2_string(ctx, q.c_str());
    Z3_error_code code = Z3_get_error_code(ctx);
    if (out.find("(error") != std::string::npos) {
      ok = false;
      reason = z3ErrorReason(out);
    } else if (code != Z3_OK) {
      ok = false;
      const char *msg = Z3_get_error_msg(ctx, code);
      reason = msg ? msg : "z3 error";
    }
    Z3_del_context(ctx);
  }
  err = reason;
  cache[tactic] = {ok, reason};
  return ok;
}

void plantZ3Error(Z3_context ctx) {
  // an ast that is not a numeral: z3 reports Z3_INVALID_ARG
  Z3_sort s = Z3_mk_bool_sort(ctx);
  Z3_ast a = Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "planted"), s);
  Z3_inc_ref(ctx, a);
  (void)Z3_get_numeral_string(ctx, a);
  Z3_dec_ref(ctx, a);
}

} // namespace sea
