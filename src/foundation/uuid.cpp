#include "am/foundation/uuid.hpp"

#include <mutex>
#include <random>

namespace am {
namespace {

std::uint64_t random_u64() {
  // std::random_device is backed by a CSPRNG on MSVC (rand_s) and libstdc++
  // (/dev/urandom or getrandom). Thread-local engine keeps it cheap.
  thread_local std::random_device rd;
  return (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

struct MonotonicState {
  std::mutex m;
  std::uint64_t last_ms = 0;
  std::uint16_t seq = 0;  // 12-bit counter in rand_a (RFC 9562 method 1)
};
MonotonicState& state() {
  static MonotonicState s;
  return s;
}

Uuid build(std::uint64_t ms, std::uint16_t rand_a12, std::uint64_t rand_b62) {
  Uuid::Bytes b{};
  for (int i = 0; i < 6; ++i) b[i] = static_cast<std::uint8_t>(ms >> (8 * (5 - i)));
  b[6] = static_cast<std::uint8_t>(0x70 | ((rand_a12 >> 8) & 0x0F));
  b[7] = static_cast<std::uint8_t>(rand_a12 & 0xFF);
  b[8] = static_cast<std::uint8_t>(0x80 | ((rand_b62 >> 56) & 0x3F));
  for (int i = 9; i < 16; ++i) b[i] = static_cast<std::uint8_t>(rand_b62 >> (8 * (15 - i)));
  return Uuid(b);
}

}  // namespace

Uuid Uuid::v4() {
  Bytes b{};
  for (int offset = 0; offset < 16; offset += 8) {
    const auto random = random_u64();
    for (int i = 0; i < 8; ++i) b[offset + i] = static_cast<std::uint8_t>(random >> (8 * i));
  }
  b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
  return Uuid(b);
}

Uuid Uuid::v7_at(std::chrono::system_clock::time_point tp) {
  auto ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count());
  ms &= 0xFFFFFFFFFFFFull;  // 48 bits
  std::uint16_t seq;
  {
    auto& s = state();
    std::lock_guard lock(s.m);
    if (ms <= s.last_ms) {
      ms = s.last_ms;                     // clock went backwards or same ms
      if (++s.seq > 0x0FFF) {             // counter overflow: borrow next ms
        ++ms;
        s.seq = 0;
      }
    } else {
      s.seq = static_cast<std::uint16_t>(random_u64() & 0x01FF);  // random start, room to grow
    }
    s.last_ms = ms;
    seq = s.seq;
  }
  return build(ms, seq, random_u64());
}

Uuid Uuid::v7() { return v7_at(std::chrono::system_clock::now()); }

std::optional<Uuid> Uuid::parse(std::string_view t) {
  if (t.size() != 36 || t[8] != '-' || t[13] != '-' || t[18] != '-' || t[23] != '-') return std::nullopt;
  Bytes b{};
  int bi = 0;
  for (std::size_t i = 0; i < t.size();) {
    if (t[i] == '-') { ++i; continue; }
    int hi = hex_value(t[i]), lo = hex_value(t[i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    b[bi++] = static_cast<std::uint8_t>((hi << 4) | lo);
    i += 2;
  }
  return Uuid(b);
}

std::optional<Uuid> Uuid::parse_canonical(std::string_view t) {
  if (t.size() != 36) return std::nullopt;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (t[i] != '-') return std::nullopt;
    } else if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'f'))) {
      return std::nullopt;
    }
  }
  return parse(t);
}

std::string Uuid::to_string() const {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  s.reserve(36);
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
    s.push_back(kHex[bytes_[i] >> 4]);
    s.push_back(kHex[bytes_[i] & 0x0F]);
  }
  return s;
}

bool Uuid::is_nil() const {
  for (auto v : bytes_) if (v != 0) return false;
  return true;
}

std::uint64_t Uuid::unix_ms() const {
  std::uint64_t ms = 0;
  for (int i = 0; i < 6; ++i) ms = (ms << 8) | bytes_[i];
  return ms;
}

std::string Uuid::shard() const { return to_string().substr(34, 2); }

}  // namespace am
