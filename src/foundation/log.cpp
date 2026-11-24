#include "am/foundation/log.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace am::log {
namespace {
std::atomic<std::shared_ptr<Sink>> sink;
std::atomic<int> minimum_level{static_cast<int>(Level::critical) + 1};
std::mutex sink_setter;
thread_local Scope* scope = nullptr;
void merge(Context& target, const Context& overlay) {
  if (overlay.job_id) target.job_id = overlay.job_id;
  if (overlay.asset_id) target.asset_id = overlay.asset_id;
  if (overlay.library_id) target.library_id = overlay.library_id;
  if (overlay.operation) target.operation = overlay.operation;
}
struct Encoder {
  std::size_t limit;
  bool repaired = false, truncated = false;
  std::string quote(std::string_view text, bool bounded = true) {
    std::string clean;
    bool cut = false;
    for (std::size_t at = 0; at < text.size();) {
      const auto lead = static_cast<unsigned char>(text[at]);
      std::size_t n = lead < 0x80                    ? 1
                      : lead >= 0xc2 && lead <= 0xdf ? 2
                      : lead >= 0xe0 && lead <= 0xef ? 3
                      : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                     : 0;
      bool valid = n && at + n <= text.size();
      for (std::size_t k = 1; valid && k < n; ++k) valid = (static_cast<unsigned char>(text[at + k]) & 0xc0) == 0x80;
      if (valid && n >= 3) {
        const auto second = static_cast<unsigned char>(text[at + 1]);
        valid = !(lead == 0xe0 && second < 0xa0) && !(lead == 0xed && second >= 0xa0) &&
                !(lead == 0xf0 && second < 0x90) && !(lead == 0xf4 && second >= 0x90);
      }
      const std::string_view part = valid ? text.substr(at, n) : std::string_view("\xef\xbf\xbd");
      if (bounded && clean.size() + part.size() > limit) {
        truncated = true;
        cut = true;
        break;
      }
      clean += part;
      at += valid ? n : 1;
      repaired |= !valid;
    }
    if (cut) {
      while (clean.size() + 3 > limit && !clean.empty()) {
        clean.pop_back();
        while (!clean.empty() && (static_cast<unsigned char>(clean.back()) & 0xc0) == 0x80) clean.pop_back();
        if (!clean.empty() && static_cast<unsigned char>(clean.back()) >= 0xc0) clean.pop_back();
      }
      if (limit >= 3) clean += "\xe2\x80\xa6";
    }
    std::string out = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char c : clean) {
      if (c == '"' || c == '\\') {
        out += '\\';
        out += static_cast<char>(c);
      } else if (c < 32) {
        out += "\\u00";
        out += digits[c >> 4];
        out += digits[c & 15];
      } else
        out += static_cast<char>(c);
    }
    return out + '"';
  }
  template <class T>
  std::string number(T value) {
    std::array<char, 128> chars{};
    auto result = std::to_chars(chars.data(), chars.data() + chars.size(), value);
    return result.ec == std::errc{} ? std::string(chars.data(), result.ptr) : "0";
  }
  std::string value(const Field& field) {
    return std::visit(
        [&](const auto& v) -> std::string {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::string>)
            return quote(v);
          else if constexpr (std::is_same_v<T, bool>)
            return v ? "true" : "false";
          else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(v)) return quote(std::isnan(v) ? "NaN" : v > 0 ? "Infinity" : "-Infinity");
            return number(v);
          } else
            return number(v);
        },
        field.value);
  }
};
std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::array<char, 40> text{};
  std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%S", &tm);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  return std::string(text.data()) + "." + static_cast<char>('0' + ms / 100) + static_cast<char>('0' + (ms / 10) % 10) +
         static_cast<char>('0' + ms % 10) + "Z";
}
std::string context_json(Encoder& e, const Context& c) {
  auto value = [&](const auto& v) { return v ? e.quote(*v) : "null"; };
  return "{\"job_id\":" + value(c.job_id) + ",\"asset_id\":" + value(c.asset_id) +
         ",\"library_id\":" + value(c.library_id) + ",\"operation\":" + value(c.operation) + "}";
}
std::string error_json(Encoder& e, const Error& error) {
  constexpr std::array severity{"info", "warning", "error", "critical"};
  constexpr std::array actions{"retry", "skip", "decide", "abort", "open_read_only", "free_space"};
  auto flat = [&](const Error& item) {
    std::string out =
        "{\"code\":" + e.quote(errc_name(item.code)) + ",\"errc\":" + e.number(static_cast<unsigned>(item.code)) +
        ",\"severity\":" + e.quote(severity[std::min<std::size_t>(static_cast<unsigned>(item.severity), 3)]) +
        ",\"component\":" + e.quote(item.component) + ",\"operation\":" + e.quote(item.ctx.operation) +
        ",\"message_id\":" + e.quote(item.message_id) + ",\"message\":" + e.quote(item.message) +
        ",\"detail\":" + e.quote(item.ctx.detail.empty() ? item.context : item.ctx.detail) + ",\"ctx\":" +
        context_json(e, {item.ctx.job_id.empty() ? std::nullopt : std::optional(item.ctx.job_id),
                         item.ctx.asset_id.empty() ? std::nullopt : std::optional(item.ctx.asset_id),
                         item.ctx.library_id.empty() ? std::nullopt : std::optional(item.ctx.library_id),
                         item.ctx.operation.empty() ? std::nullopt : std::optional(item.ctx.operation)}) +
        ",\"actions\":[";
    bool first = true;
    for (auto action : item.actions) {
      const auto i = static_cast<unsigned>(action);
      if (i >= actions.size()) continue;
      if (!first) out += ',';
      first = false;
      out += e.quote(actions[i]);
    }
    return out + "]}";
  };
  auto out = flat(error);
  out.pop_back();
  out += ",\"cause\":[";
  auto next = error.cause.get();
  unsigned depth = 0;
  while (next && depth < 16) {
    if (depth++) out += ',';
    out += flat(*next);
    next = next->cause.get();
  }
  out += ']';
  if (next) out += ",\"cause_truncated\":true";
  return out + '}';
}
}  // namespace
std::string_view level_name(Level level) noexcept {
  constexpr std::array values{"trace", "debug", "info", "warning", "error", "critical"};
  const auto i = static_cast<unsigned>(level);
  return i < values.size() ? values[i] : "error";
}
void set_sink(std::shared_ptr<Sink> value) noexcept {
  const auto minimum = value ? static_cast<int>(value->min_level()) : static_cast<int>(Level::critical) + 1;
  std::shared_ptr<Sink> previous;
  {
    const std::lock_guard lock(sink_setter);
    previous = sink.exchange(std::move(value), std::memory_order_acq_rel);
    minimum_level.store(minimum, std::memory_order_relaxed);
  }
}
bool enabled(Level level) noexcept { return static_cast<int>(level) >= minimum_level.load(std::memory_order_relaxed); }
Context current_context() { return scope ? scope->context_ : Context{}; }
Scope::Scope(Context value) noexcept {
  try {
    context_ = current_context();
    merge(context_, value);
    previous_ = scope;
    scope = this;
    installed_ = true;
  } catch (...) {
  }
}
Scope::~Scope() {
  if (installed_) scope = previous_;
}
void write(const Record& record) noexcept {
  try {
    auto target = sink.load(std::memory_order_acquire);
    if (!target || record.level < target->min_level()) return;
    auto copy = record;
    copy.context = current_context();
    merge(copy.context, record.context);
    target->write(copy);
  } catch (...) {
  }
}
void write(Level level, std::string_view component, std::string_view event, std::string_view message,
           std::initializer_list<Field> fields, const Error* error) noexcept {
  write(Record{level, component, event, message, fields, error, {}});
}
std::string serialize(const Record& record, const RecordLimits& limits) noexcept {
  try {
    std::size_t text_limit = limits.text_bytes, field_limit = record.fields.size();
    bool budget_cut = false;
    const auto ts = timestamp();
#ifdef _WIN32
    const auto pid = GetCurrentProcessId(), tid = GetCurrentThreadId();
#else
    const auto pid = ::getpid();
    const auto tid = ::syscall(SYS_gettid);
#endif
    for (;;) {
      Encoder e{text_limit};
      e.truncated = budget_cut;
      std::string out = "{\"v\":1,\"ts\":" + e.quote(ts, false) +
                        ",\"level\":" + e.quote(level_name(record.level), false) +
                        ",\"component\":" + e.quote(record.component) + ",\"event\":" + e.quote(record.event) +
                        ",\"msg\":" + e.quote(record.message) + ",\"pid\":" + e.number(pid) +
                        ",\"tid\":" + e.number(tid) + ",\"ctx\":" + context_json(e, record.context) + ",\"fields\":{";
      std::map<std::string_view, const Field*> fields;
      for (const auto& f : record.fields) {
        bool valid = !f.key.empty() && f.key[0] >= 'a' && f.key[0] <= 'z';
        for (char c : f.key) valid &= (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (valid && f.key.size() <= limits.text_bytes)
          fields[f.key] = &f;
        else {
          (void)e.quote(f.key);
          e.truncated = true;
        }
      }
      std::string kinds = "{";
      bool first = true, first_kind = true;
      std::size_t written = 0;
      for (const auto& [key, field] : fields) {
        if (written++ >= field_limit) {
          e.truncated = true;
          break;
        }
        if (!first) out += ',';
        first = false;
        out += e.quote(key, false) + ":" + e.value(*field);
        if (field->kind != FieldKind::plain) {
          if (!first_kind) kinds += ',';
          first_kind = false;
          kinds += e.quote(key, false) + ":" + e.quote(field->kind == FieldKind::path ? "path" : "name", false);
        }
      }
      out += "},\"kinds\":" + kinds + "}";
      if (record.error) out += ",\"error\":" + error_json(e, *record.error);
      if (e.repaired) out += ",\"utf8_repaired\":true";
      if (e.truncated) out += ",\"truncated\":true";
      out += "}\n";
      if (out.size() <= limits.record_bytes) return out;
      budget_cut = true;
      if (field_limit)
        field_limit /= 2;
      else if (text_limit)
        text_limit /= 2;
      else
        return {};
    }
  } catch (...) {
    return {};
  }
}
}  // namespace am::log
