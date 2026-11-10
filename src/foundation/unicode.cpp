#include "am/foundation/unicode.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <new>
#include <stdexcept>
#include <utility>

#include "am/foundation/text_encoding.hpp"

namespace am::unicode {
namespace {

struct CombiningClass { char32_t scalar; unsigned value; };
struct Decomposition { char32_t scalar; std::size_t offset; std::size_t length; };
struct Composition { char32_t first; char32_t second; char32_t composite; };
struct CaseFold { char32_t scalar; char32_t target; };

#include "unicode_tables.inc"

// Hangul constants from Unicode 17.0.0, section 3.12.
constexpr char32_t kSBase = 0xAC00, kLBase = 0x1100, kVBase = 0x1161, kTBase = 0x11A7;
constexpr char32_t kLCount = 19, kVCount = 21, kTCount = 28;
constexpr char32_t kNCount = kVCount * kTCount, kSCount = kLCount * kNCount;

template <class T, std::size_t N>
const T* lookup(const T (&table)[N], char32_t cp) {
  const auto it = std::lower_bound(std::begin(table), std::end(table), cp,
                                 [](const T& row, char32_t value) { return row.scalar < value; });
  return it != std::end(table) && it->scalar == cp ? it : nullptr;
}

unsigned combining_class(char32_t cp) {
  const auto row = lookup(kCombiningClasses, cp);
  return row ? row->value : 0;
}

// Called only after strict validation of the entire input.
char32_t next_scalar(std::string_view s, std::size_t& offset) {
  const auto lead = static_cast<unsigned char>(s[offset++]);
  if (lead < 0x80) return lead;
  const unsigned length = lead < 0xE0 ? 2u : lead < 0xF0 ? 3u : 4u;
  char32_t cp = lead & (0x7Fu >> length);
  for (unsigned i = 1; i < length; ++i) {
    cp = (cp << 6) | (static_cast<unsigned char>(s[offset++]) & 0x3Fu);
  }
  return cp;
}

void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

void decompose(std::u32string& out, char32_t cp) {
  if (cp >= kSBase && cp < kSBase + kSCount) {
    const auto index = cp - kSBase;
    out.push_back(kLBase + index / kNCount);
    out.push_back(kVBase + (index % kNCount) / kTCount);
    if (index % kTCount) out.push_back(kTBase + index % kTCount);
  } else if (const auto row = lookup(kDecompositions, cp)) {
    out.append(kDecompositionScalars + row->offset, row->length);
  } else {
    out.push_back(cp);
  }
}

char32_t compose(char32_t a, char32_t b) {
  if (a >= kLBase && a < kLBase + kLCount && b >= kVBase && b < kVBase + kVCount)
    return kSBase + (a - kLBase) * kNCount + (b - kVBase) * kTCount;
  if (a >= kSBase && a < kSBase + kSCount && (a - kSBase) % kTCount == 0 &&
      b > kTBase && b < kTBase + kTCount)
    return a + b - kTBase;
  const auto key = std::pair(a, b);
  const auto it = std::lower_bound(std::begin(kCompositions), std::end(kCompositions), key,
                                  [](const Composition& row, const auto& value) {
                                    return std::pair(row.first, row.second) < value;
                                  });
  return it != std::end(kCompositions) && it->first == a && it->second == b ? it->composite : 0;
}

std::string normalize_valid(std::string_view utf8) {
  std::u32string scalars;
  scalars.reserve(utf8.size());
  for (std::size_t i = 0; i < utf8.size();) decompose(scalars, next_scalar(utf8, i));

  // Stable ordering of each non-starter run, including a leading run.
  for (std::size_t i = 0; i < scalars.size();) {
    if (combining_class(scalars[i]) == 0) { ++i; continue; }
    const auto begin = i;
    while (i < scalars.size() && combining_class(scalars[i]) != 0) ++i;
    if (i - begin > 1) {
      std::stable_sort(scalars.begin() + static_cast<std::ptrdiff_t>(begin),
                       scalars.begin() + static_cast<std::ptrdiff_t>(i),
                       [](char32_t a, char32_t b) { return combining_class(a) < combining_class(b); });
    }
  }

  // Compose in place. A consumed mark does not update the blocking class.
  std::size_t written = 0, starter = std::u32string::npos;
  unsigned last_class = 0;
  for (const auto cp : scalars) {
    const auto current_class = combining_class(cp);
    if (starter != std::u32string::npos && (last_class == 0 || last_class < current_class)) {
      if (const auto composite = compose(scalars[starter], cp)) {
        scalars[starter] = composite;
        continue;
      }
    }
    if (current_class == 0) starter = written;
    scalars[written++] = cp;
    last_class = current_class;
  }
  std::string out;
  out.reserve(utf8.size());
  for (std::size_t i = 0; i < written; ++i) append_utf8(out, scalars[i]);
  return out;
}

std::string fold_valid(std::string_view utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (std::size_t i = 0; i < utf8.size();) {
    const auto cp = next_scalar(utf8, i);
    const auto row = lookup(kCaseFolds, cp);
    append_utf8(out, row ? row->target : cp);
  }
  return out;
}

Error allocation_error() {
  // Do not allocate again while handling allocation/length failure.
  Error error;
  error.code = Errc::resource_exhausted;
  return error;
}

Error invalid_utf8() {
  return Error::make(Errc::invalid_argument, "Invalid UTF-8").in("foundation.unicode").id("unicode.invalid_utf8");
}

}  // namespace

bool is_valid_utf8(std::string_view s) {
  return text::is_valid_utf8({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
}

Result<std::string> nfc(std::string_view utf8) {
  try {
    if (!is_valid_utf8(utf8)) return invalid_utf8();
    return normalize_valid(utf8);
  } catch (const std::bad_alloc&) {
    return allocation_error();
  } catch (const std::length_error&) {
    return allocation_error();
  }
}

Result<std::string> simple_casefold(std::string_view utf8) {
  try {
    if (!is_valid_utf8(utf8)) return invalid_utf8();
    return fold_valid(utf8);
  } catch (const std::bad_alloc&) {
    return allocation_error();
  } catch (const std::length_error&) {
    return allocation_error();
  }
}

std::string path_collision_key(std::string_view s) {
  try {
    if (is_valid_utf8(s)) return normalize_valid(fold_valid(normalize_valid(s)));
    std::string out(s);
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
  } catch (const std::bad_alloc&) {
    return {};
  } catch (const std::length_error&) {
    return {};
  }
}

}  // namespace am::unicode
