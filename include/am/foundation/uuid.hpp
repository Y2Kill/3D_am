#pragma once
// UUIDv7 (RFC 9562) identifiers for assets, libraries, operations (ADR-0008).

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace am {

class Uuid {
 public:
  using Bytes = std::array<std::uint8_t, 16>;

  constexpr Uuid() = default;
  explicit constexpr Uuid(const Bytes& b) : bytes_(b) {}

  // New time-ordered UUIDv7. Monotonic within one process: ids created in the
  // same millisecond still sort in creation order.
  static Uuid v7();
  static Uuid v7_at(std::chrono::system_clock::time_point tp);  // for tests

  // Accepts canonical 8-4-4-4-12 form, case-insensitive.
  static std::optional<Uuid> parse(std::string_view text);

  std::string to_string() const;   // lower-case canonical form, 36 chars
  int version() const { return bytes_[6] >> 4; }
  bool is_nil() const;
  // Milliseconds since Unix epoch for v7 ids.
  std::uint64_t unix_ms() const;
  // Last two hex digits — used for library sharding assets/<xx>/ (ADR-0007).
  std::string shard() const;

  const Bytes& bytes() const { return bytes_; }
  auto operator<=>(const Uuid&) const = default;

 private:
  Bytes bytes_{};
};

}  // namespace am
