// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Boost is used with exceptions disabled (LLVM builds with -fno-exceptions),
// so Boost calls these instead of throwing. Ported from SeaHorn; reports the
// error instead of exiting silently.
#include <boost/version.hpp>
#include <cstdio>
#include <cstdlib>
#include <exception>

#if BOOST_VERSION / 100 % 100 >= 73
#include <boost/assert/source_location.hpp>
#endif

#ifdef BOOST_NO_EXCEPTIONS
namespace boost {
void throw_exception(std::exception const &e) {
  std::fprintf(stderr, "fatal: boost exception: %s\n", e.what());
  std::abort();
}
#if BOOST_VERSION / 100 % 100 >= 73
// Starting with boost 1.73
void throw_exception(std::exception const &e,
                     boost::source_location const &loc) {
  std::fprintf(stderr, "fatal: boost exception: %s (%s:%u)\n", e.what(),
               loc.file_name(), (unsigned)loc.line());
  std::abort();
}
#endif
} // namespace boost
#endif
