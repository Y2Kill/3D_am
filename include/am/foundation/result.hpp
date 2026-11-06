#pragma once
// Error handling for all layers: no exceptions across module boundaries and
// never across the plugin C ABI. Result<T> is a minimal std::expected stand-in
// (std::expected is C++23; ADR-0001 keeps public headers at C++20).

#include <string>
#include <utility>
#include <variant>

namespace am {

enum class Errc {
  ok = 0,
  invalid_argument,
  not_found,
  io_error,
  corrupt_data,        // damaged archive / container / manifest
  unsupported,         // unknown format, newer container version
  conflict,            // ID/revision conflict, external modification
  cancelled,
  needs_decision,      // job must ask the user (ADR-0015)
  permission_denied,   // operation exceeds granted level (ADR-0027)
  internal,
};

struct Error {
  Errc code = Errc::internal;
  std::string message;   // human-readable, UTF-8
  std::string context;   // e.g. path, entry name, stage

  static Error make(Errc c, std::string msg, std::string ctx = {}) {
    return Error{c, std::move(msg), std::move(ctx)};
  }
};

template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : v_(std::move(value)) {}            // NOLINT(google-explicit-constructor)
  Result(Error error) : v_(std::move(error)) {}        // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return v_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  T& value() & { return std::get<0>(v_); }
  const T& value() const& { return std::get<0>(v_); }
  T&& value() && { return std::get<0>(std::move(v_)); }
  T* operator->() { return &std::get<0>(v_); }
  const T* operator->() const { return &std::get<0>(v_); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }

  const Error& error() const { return std::get<1>(v_); }

  template <class U>
  T value_or(U&& fallback) const& { return ok() ? value() : static_cast<T>(std::forward<U>(fallback)); }

 private:
  std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : err_(std::move(error)), has_err_(true) {}  // NOLINT(google-explicit-constructor)
  bool ok() const noexcept { return !has_err_; }
  explicit operator bool() const noexcept { return ok(); }
  const Error& error() const { return err_; }

 private:
  Error err_{};
  bool has_err_ = false;
};

}  // namespace am
