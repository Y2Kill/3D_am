#pragma once
// Decoding of archive entry names (ADR-0012).
//
// Inventory finding: ~1.2k ZIP entry names in the reference collection are
// UTF-8 bytes stored WITHOUT the UTF-8 flag (bit 11); tools that fall back to
// CP437 show "ΓÇô" instead of "–". Order of detection used by the importer:
//   1. general-purpose flag bit 11 set          -> UTF-8
//   2. Info-ZIP Unicode Path extra field 0x7075 -> UTF-8 from extra field
//   3. bytes are strictly valid UTF-8            -> UTF-8
//   4. statistical guess among CP866/CP1251/CP437 (this file)
//   5. user override per archive
// The raw bytes are always kept in the manifest when (3) fails.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace am::text {

enum class Codepage { utf8, cp437, cp866, cp1251 };

std::string_view codepage_name(Codepage cp);

// Strict UTF-8 validation (rejects overlongs, surrogates, > U+10FFFF).
bool is_valid_utf8(std::span<const std::uint8_t> bytes);
inline bool is_valid_utf8(std::string_view s) {
  return is_valid_utf8({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
}
bool is_ascii(std::span<const std::uint8_t> bytes);

// Single-byte codepage -> UTF-8. Never fails (every byte maps to a code point).
std::string decode(std::span<const std::uint8_t> bytes, Codepage cp);

struct NameGuess {
  Codepage codepage = Codepage::utf8;
  std::string utf8;          // decoded name
  bool exact = false;        // true for ASCII / valid UTF-8; false for statistical guesses
  double confidence = 1.0;   // 0..1, only meaningful when !exact
};

// Steps 3–4 of the detection order above.
NameGuess guess_name_encoding(std::span<const std::uint8_t> raw);

// Repairs a string that was produced by decoding UTF-8 bytes as CP437
// ("ΓÇô" -> "–"). Returns the input unchanged when it is not such mojibake.
// Useful to post-process inventories made by tools like Python's zipfile.
std::string repair_cp437_mojibake(std::string_view utf8);

// Encode UTF-8 text back to a single-byte codepage; returns false if some
// character is not representable.
bool encode(std::string_view utf8, Codepage cp, std::string& out_bytes);

std::string to_base64(std::span<const std::uint8_t> bytes);

}  // namespace am::text
