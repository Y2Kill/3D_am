#pragma once
#include <atomic>
#include <initializer_list>
#include <optional>
#include <span>

#include "am/foundation/result.hpp"

namespace am::log {
enum class Level { trace, debug, info, warning, error, critical };
enum class FieldKind { plain, path, name };
struct Field {
  std::string key;
  std::variant<std::string, std::int64_t, std::uint64_t, bool, double> value;
  FieldKind kind = FieldKind::plain;
};
struct Context {
  std::optional<std::string> job_id, asset_id, library_id, operation;
};
struct Record {
  Level level = Level::info;
  std::string_view component, event, message;
  std::span<const Field> fields;
  const Error* error = nullptr;
  Context context;
};
class Sink {
 public:
  virtual ~Sink() = default;
  virtual Level min_level() const noexcept = 0;
  virtual void write(const Record&) noexcept = 0;
};
void set_sink(std::shared_ptr<Sink>) noexcept;
bool enabled(Level) noexcept;
void write(const Record&) noexcept;
void write(Level, std::string_view component, std::string_view event, std::string_view message,
           std::initializer_list<Field> = {}, const Error* = nullptr) noexcept;
class Scope {
 public:
  explicit Scope(Context) noexcept;
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Context context_;
  Scope* previous_ = nullptr;
  bool installed_ = false;
  friend Context current_context();
};
Context current_context();
std::string_view level_name(Level) noexcept;
struct RecordLimits {
  // Hypotheses; not performance requirements.
  std::size_t text_bytes = 4 * 1024, record_bytes = 64 * 1024;
};
// An empty result means serialization failed or the configured record budget is too small.
std::string serialize(const Record&, const RecordLimits& = {}) noexcept;
}  // namespace am::log
#define AM_LOG(level, ...)                                      \
  do {                                                          \
    if (::am::log::enabled(::am::log::Level::level)) {          \
      try {                                                     \
        ::am::log::write(::am::log::Level::level, __VA_ARGS__); \
      } catch (...) {                                           \
      }                                                         \
    }                                                           \
  } while (false)
