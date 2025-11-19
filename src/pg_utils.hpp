#pragma once

extern "C" {
#include "postgres.h"
#include "utils/elog.h"
#include "utils/memutils.h"
}

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace pg {

class postgres_exception : public std::runtime_error {
 public:
  explicit postgres_exception(const std::string& msg) : std::runtime_error(msg) {}
};

template <typename Func>
auto pg_try(Func&& func) -> decltype(func()) {
  using ReturnType = decltype(func());
  MemoryContext old_context = CurrentMemoryContext;

  if constexpr (std::is_void_v<ReturnType>) {
    std::optional<std::string> pg_error_msg;
    std::exception_ptr cpp_exception;

    PG_TRY();
    {
      try {
        std::forward<Func>(func)();
      } catch (...) {
        // Catch C++ exceptions and rethrow later to ensure PG_END_TRY() is called
        cpp_exception = std::current_exception();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);

      ErrorData* edata = CopyErrorData();
      FlushErrorState();

      pg_error_msg = "PostgreSQL error";
      if (edata->message) {
        *pg_error_msg += ": ";
        *pg_error_msg += edata->message;
      }
      FreeErrorData(edata);
    }
    PG_END_TRY();

    if (cpp_exception) {
      std::rethrow_exception(cpp_exception);
    }

    if (pg_error_msg.has_value()) {
      throw postgres_exception(*pg_error_msg);
    }
  } else {
    std::optional<ReturnType> result;
    std::optional<std::string> pg_error_msg;
    std::exception_ptr cpp_exception;

    PG_TRY();
    {
      try {
        result = std::forward<Func>(func)();
      } catch (...) {
        // Catch C++ exceptions and rethrow later to ensure PG_END_TRY() is called
        cpp_exception = std::current_exception();
      }
    }
    PG_CATCH();
    {
      MemoryContextSwitchTo(old_context);

      ErrorData* edata = CopyErrorData();
      FlushErrorState();

      pg_error_msg = "PostgreSQL error";
      if (edata->message) {
        *pg_error_msg += ": ";
        *pg_error_msg += edata->message;
      }
      FreeErrorData(edata);
    }
    PG_END_TRY();

    if (cpp_exception) {
      std::rethrow_exception(cpp_exception);
    }

    if (pg_error_msg.has_value()) {
      throw postgres_exception(*pg_error_msg);
    }

    return std::move(*result);
  }
}

template <typename Func>
void pg_assert_noerror(Func&& func, const char* context) noexcept {
  MemoryContext old_context = CurrentMemoryContext;

  PG_TRY();
  {
    std::forward<Func>(func)();
  }
  PG_CATCH();
  {
    MemoryContextSwitchTo(old_context);

    ErrorData* edata = CopyErrorData();

    elog(FATAL, "Unexpected PostgreSQL error in %s: %s", context, edata->message ? edata->message : "(no message)");
    Assert(false);

    FlushErrorState();
    FreeErrorData(edata);
  }
  PG_END_TRY();
}

}  // namespace pg
