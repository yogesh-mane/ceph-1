#include <cstdarg>

#include <seastar/util/backtrace.hh>
#include <seastar/core/reactor.hh>

#include "include/assert.h"

#include "crimson/common/log.h"

namespace ceph {
  [[gnu::cold]] void __ceph_assert_fail(const ceph::assert_data &ctx)
  {
    __ceph_assert_fail(ctx.assertion, ctx.file, ctx.line, ctx.function);
  }

  [[gnu::cold]] void __ceph_assert_fail(const char* assertion,
                                        const char* file, int line,
                                        const char* func)
  {
    seastar::logger& logger = ceph::get_logger(0);
    logger.error("{}:{} : In function '{}', ceph_assert(%s)\n"
                 "{}",
                 file, line, func, assertion,
                 seastar::current_backtrace());
    abort();
  }
  [[gnu::cold]] void __ceph_assertf_fail(const char *assertion,
                                         const char *file, int line,
                                         const char *func, const char* msg,
                                         ...)
  {
    char buf[8096];
    va_list args;
    va_start(args, msg);
    std::vsnprintf(buf, sizeof(buf), msg, args);
    va_end(args);

    seastar::logger& logger = ceph::get_logger(0);
    logger.error("{}:{} : In function '{}', ceph_assert(%s)\n"
                 "{}",
                 file, line, func, assertion,
                 seastar::current_backtrace());
    abort();
  }

  [[gnu::cold]] void __ceph_abort(const char* file, int line,
                                  const char* func, const std::string& msg)
  {
    seastar::logger& logger = ceph::get_logger(0);
    logger.error("{}:{} : In function '{}', abort(%s)\n"
                 "{}",
                 file, line, func, msg,
                 seastar::current_backtrace());
    abort();
  }
}
