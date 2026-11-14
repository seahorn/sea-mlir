// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "sea/Expr/Smt/Z3Error.h"

#include "sea/Support/Diag.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace sea {

namespace {
thread_local unsigned expectDepth = 0;

void z3ErrorHandler(Z3_context ctx, Z3_error_code code) {
  if (expectDepth)
    return;
  const char *msg = Z3_get_error_msg(ctx, code);
  sea::diag::error("smt.z3-internal",
                   llvm::Twine("z3 reported an error (code ") +
                       llvm::Twine(static_cast<int>(code)) +
                       "): " + (msg ? msg : "no message"));
  sea::diag::printSummary();
  llvm::outs().flush();
  llvm::errs().flush();
  // not exit(): z3 is mid-call, and static destructors would run under it
  _exit(1);
}
} // namespace

void installZ3ErrorHandler(Z3_context ctx) {
  Z3_set_error_handler(ctx, z3ErrorHandler);
}

Z3ExpectErrors::Z3ExpectErrors() { ++expectDepth; }
Z3ExpectErrors::~Z3ExpectErrors() { --expectDepth; }

bool z3SetGlobalParam(const std::string &name, const std::string &value,
                      std::string &err) {
  // z3 writes its complaint (unknown name, bad value) to stderr and goes on:
  // read it from there
  FILE *tmp = std::tmpfile();
  if (!tmp) {
    Z3_global_param_set(name.c_str(), value.c_str());
    return true;
  }
  llvm::errs().flush();
  fflush(stderr);
  int saved = dup(2);
  dup2(fileno(tmp), 2);
  Z3_global_param_set(name.c_str(), value.c_str());
  fflush(stderr);
  dup2(saved, 2);
  close(saved);
  std::string msg;
  rewind(tmp);
  char buf[512];
  while (size_t n = fread(buf, 1, sizeof buf, tmp))
    msg.append(buf, n);
  fclose(tmp);
  if (msg.empty())
    return true;
  // the first line only (z3 follows an unknown name with the list of names)
  size_t nl = msg.find('\n');
  err = msg.substr(0, nl);
  if (err.rfind("WARNING: ", 0) == 0)
    err.erase(0, 9);
  return false;
}

bool z3CheckTactic(const std::string &tactic, std::string &err) {
  Z3_config cfg = Z3_mk_config();
  Z3_context ctx = Z3_mk_context(cfg);
  Z3_del_config(cfg);
  // the answer is read from the output: an error is the caller's to report
  Z3ExpectErrors expect;
  installZ3ErrorHandler(ctx);
  std::string q = "(declare-const x Bool)(assert x)(check-sat-using " + tactic +
                  ")";
  std::string out = Z3_eval_smtlib2_string(ctx, q.c_str());
  Z3_del_context(ctx);
  size_t at = out.find("(error");
  if (at == std::string::npos)
    return true;
  err = out.substr(at);
  while (!err.empty() && (err.back() == '\n' || err.back() == ' '))
    err.pop_back();
  return false;
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
