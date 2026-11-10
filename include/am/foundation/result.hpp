#pragma once
// Error handling for all layers: no exceptions across module boundaries and
// never across the plugin C ABI. Result<T> is a minimal std::expected stand-in
// (std::expected is C++23; ADR-0001 keeps public headers at C++20).
//
// Structured error model (TZ §14.4.2, ADR-0032):
//   stable code + severity + component/operation + Job/Asset/LibraryID context
//   + technical cause + message_id for localized UI text + allowed actions.
// Codes are never tied to localized messages.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace am {

// STABLE numeric values: never renumber or reuse a value (ADR-0032).
// New codes are appended with the next free number.
enum class Errc : std::uint16_t {
  ok = 0,
  invalid_argument = 1,
  not_found = 2,
  io_error = 3,
  corrupt_data = 4,         // damaged archive / container / manifest
  unsupported = 5,          // unknown format, newer container version
  conflict = 6,             // ID/revision conflict, external modification
  cancelled = 7,
  needs_decision = 8,       // job must ask the user (ADR-0015)
  permission_denied = 9,    // operation exceeds granted level (ADR-0027)
  internal = 10,
  resource_exhausted = 11,  // RAM/VRAM/handles/budget exceeded (TZ §14.2, ADR-0033)
  insufficient_space = 12,  // not enough disk space for a safe transaction (ADR-0033)
  read_only = 13,           // library/medium opened read-only (ADR-0031)
  locked = 14,              // write owner is another process (ADR-0031)
  incomplete = 15,          // import/validation finished with known gaps (ADR-0035 p.15)
};

// Stable machine name, e.g. "am.insufficient_space". Not localized.
std::string_view errc_name(Errc c) noexcept;

enum class Severity : std::uint8_t { info, warning, error, critical };

// What the caller/UI may offer the user. Not every action fits every error.
enum class ErrorAction : std::uint8_t { retry, skip, decide, abort, open_read_only, free_space };

struct ErrorContext {
  std::string operation;   // e.g. "asset.commit", "import.scan"
  std::string job_id;      // textual IDs: foundation must not depend on domain types
  std::string asset_id;
  std::string library_id;
  std::string detail;      // path, entry name, stage
};

struct Error {
  Errc code = Errc::internal;
  std::string message;     // technical description, UTF-8, English; NOT the UI text
  std::string context;     // legacy short context (path, entry); mirrored in ctx.detail
  Severity severity = Severity::error;
  std::string component;   // e.g. "storage", "processing.formats.stl"
  std::string message_id;  // stable key for localized UI text, e.g. "stl.too_small"
  ErrorContext ctx;
  std::vector<ErrorAction> actions;
  std::shared_ptr<const Error> cause;  // underlying error (chain), may be null

  static Error make(Errc c, std::string msg, std::string ctx_detail = {}) {
    Error e;
    e.code = c;
    e.message = std::move(msg);
    e.context = ctx_detail;
    e.ctx.detail = std::move(ctx_detail);
    return e;
  }

  Error&& in(std::string comp) && { component = std::move(comp); return std::move(*this); }
  Error&& op(std::string operation) && { ctx.operation = std::move(operation); return std::move(*this); }
  Error&& id(std::string mid) && { message_id = std::move(mid); return std::move(*this); }
  Error&& with_severity(Severity s) && { severity = s; return std::move(*this); }
  Error&& allow(ErrorAction a) && { actions.push_back(a); return std::move(*this); }
  Error&& for_job(std::string v) && { ctx.job_id = std::move(v); return std::move(*this); }
  Error&& for_asset(std::string v) && { ctx.asset_id = std::move(v); return std::move(*this); }
  Error&& for_library(std::string v) && { ctx.library_id = std::move(v); return std::move(*this); }
  Error&& caused_by(Error inner) && {
    cause = std::make_shared<const Error>(std::move(inner));
    return std::move(*this);
  }

  bool allows(ErrorAction a) const noexcept {
    for (auto x : actions) if (x == a) return true;
    return false;
  }
  // Deepest error in the cause chain (the original technical reason).
  const Error& root_cause() const noexcept {
    const Error* e = this;
    while (e->cause) e = e->cause.get();
    return *e;
  }
};

// One-line technical rendering for logs/CLI: "am.io_error [storage] asset.commit: cannot open (x.zip) <- ...".
std::string to_log_string(const Error& e);

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
