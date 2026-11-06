#include "am/foundation/text_encoding.hpp"

#include <array>

namespace am::text {
namespace {

// Upper halves (0x80..0xFF) of the supported single-byte code pages.
constexpr std::array<char16_t, 128> kCp437 = {
    u'Ç', u'ü', u'é', u'â', u'ä', u'à', u'å', u'ç', u'ê', u'ë', u'è', u'ï', u'î', u'ì', u'Ä', u'Å',
    u'É', u'æ', u'Æ', u'ô', u'ö', u'ò', u'û', u'ù', u'ÿ', u'Ö', u'Ü', u'¢', u'£', u'¥', u'₧', u'ƒ',
    u'á', u'í', u'ó', u'ú', u'ñ', u'Ñ', u'ª', u'º', u'¿', u'⌐', u'¬', u'½', u'¼', u'¡', u'«', u'»',
    u'░', u'▒', u'▓', u'│', u'┤', u'╡', u'╢', u'╖', u'╕', u'╣', u'║', u'╗', u'╝', u'╜', u'╛', u'┐',
    u'└', u'┴', u'┬', u'├', u'─', u'┼', u'╞', u'╟', u'╚', u'╔', u'╩', u'╦', u'╠', u'═', u'╬', u'╧',
    u'╨', u'╤', u'╥', u'╙', u'╘', u'╒', u'╓', u'╫', u'╪', u'┘', u'┌', u'█', u'▄', u'▌', u'▐', u'▀',
    u'α', u'ß', u'Γ', u'π', u'Σ', u'σ', u'µ', u'τ', u'Φ', u'Θ', u'Ω', u'δ', u'∞', u'φ', u'ε', u'∩',
    u'≡', u'±', u'≥', u'≤', u'⌠', u'⌡', u'÷', u'≈', u'°', u'∙', u'·', u'√', u'ⁿ', u'²', u'■', u' ',
};

constexpr std::array<char16_t, 128> kCp866 = {
    u'А', u'Б', u'В', u'Г', u'Д', u'Е', u'Ж', u'З', u'И', u'Й', u'К', u'Л', u'М', u'Н', u'О', u'П',
    u'Р', u'С', u'Т', u'У', u'Ф', u'Х', u'Ц', u'Ч', u'Ш', u'Щ', u'Ъ', u'Ы', u'Ь', u'Э', u'Ю', u'Я',
    u'а', u'б', u'в', u'г', u'д', u'е', u'ж', u'з', u'и', u'й', u'к', u'л', u'м', u'н', u'о', u'п',
    u'░', u'▒', u'▓', u'│', u'┤', u'╡', u'╢', u'╖', u'╕', u'╣', u'║', u'╗', u'╝', u'╜', u'╛', u'┐',
    u'└', u'┴', u'┬', u'├', u'─', u'┼', u'╞', u'╟', u'╚', u'╔', u'╩', u'╦', u'╠', u'═', u'╬', u'╧',
    u'╨', u'╤', u'╥', u'╙', u'╘', u'╒', u'╓', u'╫', u'╪', u'┘', u'┌', u'█', u'▄', u'▌', u'▐', u'▀',
    u'р', u'с', u'т', u'у', u'ф', u'х', u'ц', u'ч', u'ш', u'щ', u'ъ', u'ы', u'ь', u'э', u'ю', u'я',
    u'Ё', u'ё', u'Є', u'є', u'Ї', u'ї', u'Ў', u'ў', u'°', u'∙', u'·', u'√', u'№', u'¤', u'■', u' ',
};

constexpr std::array<char16_t, 128> kCp1251 = {
    u'Ђ', u'Ѓ', u'‚', u'ѓ', u'„', u'…', u'†', u'‡', u'€', u'‰', u'Љ', u'‹', u'Њ', u'Ќ', u'Ћ', u'Џ',
    u'ђ', u'‘', u'’', u'“', u'”', u'•', u'–', u'—', u'�', u'™', u'љ', u'›', u'њ', u'ќ', u'ћ', u'џ',
    u' ', u'Ў', u'ў', u'Ј', u'¤', u'Ґ', u'¦', u'§', u'Ё', u'©', u'Є', u'«', u'¬', u'­', u'®', u'Ї',
    u'°', u'±', u'І', u'і', u'ґ', u'µ', u'¶', u'·', u'ё', u'№', u'є', u'»', u'ј', u'Ѕ', u'ѕ', u'ї',
    u'А', u'Б', u'В', u'Г', u'Д', u'Е', u'Ж', u'З', u'И', u'Й', u'К', u'Л', u'М', u'Н', u'О', u'П',
    u'Р', u'С', u'Т', u'У', u'Ф', u'Х', u'Ц', u'Ч', u'Ш', u'Щ', u'Ъ', u'Ы', u'Ь', u'Э', u'Ю', u'Я',
    u'а', u'б', u'в', u'г', u'д', u'е', u'ж', u'з', u'и', u'й', u'к', u'л', u'м', u'н', u'о', u'п',
    u'р', u'с', u'т', u'у', u'ф', u'х', u'ц', u'ч', u'ш', u'щ', u'ъ', u'ы', u'ь', u'э', u'ю', u'я',
};

const std::array<char16_t, 128>& table(Codepage cp) {
  switch (cp) {
    case Codepage::cp866: return kCp866;
    case Codepage::cp1251: return kCp1251;
    default: return kCp437;
  }
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

// Decodes one code point from valid UTF-8; advances i. Returns U+FFFD on error.
char32_t next_cp(std::string_view s, std::size_t& i) {
  auto c = static_cast<unsigned char>(s[i]);
  int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
  if (len == 0 || i + len > s.size()) { ++i; return 0xFFFD; }
  char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
  for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
  i += len;
  return cp;
}

}  // namespace

std::string_view codepage_name(Codepage cp) {
  switch (cp) {
    case Codepage::utf8: return "utf-8";
    case Codepage::cp437: return "cp437";
    case Codepage::cp866: return "cp866";
    case Codepage::cp1251: return "cp1251";
  }
  return "?";
}

bool is_ascii(std::span<const std::uint8_t> b) {
  for (auto c : b) if (c >= 0x80) return false;
  return true;
}

bool is_valid_utf8(std::span<const std::uint8_t> b) {
  std::size_t i = 0, n = b.size();
  while (i < n) {
    std::uint8_t c = b[i];
    if (c < 0x80) { ++i; continue; }
    std::size_t len;
    char32_t min;
    if ((c & 0xE0) == 0xC0) { len = 2; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; min = 0x10000; }
    else return false;
    if (i + len > n) return false;
    char32_t cp = c & (0x7F >> len);
    for (std::size_t k = 1; k < len; ++k) {
      if ((b[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (b[i + k] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

std::string decode(std::span<const std::uint8_t> bytes, Codepage cp) {
  std::string out;
  if (cp == Codepage::utf8) {
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return out;
  }
  out.reserve(bytes.size() * 2);
  const auto& t = table(cp);
  for (auto c : bytes) {
    if (c < 0x80) out.push_back(static_cast<char>(c));
    else append_utf8(out, t[c - 0x80]);
  }
  return out;
}

bool encode(std::string_view utf8, Codepage cp, std::string& out) {
  out.clear();
  if (cp == Codepage::utf8) { out.assign(utf8); return true; }
  const auto& t = table(cp);
  for (std::size_t i = 0; i < utf8.size();) {
    char32_t c = next_cp(utf8, i);
    if (c < 0x80) { out.push_back(static_cast<char>(c)); continue; }
    bool found = false;
    for (std::size_t k = 0; k < 128; ++k) {
      if (t[k] == c) { out.push_back(static_cast<char>(0x80 + k)); found = true; break; }
    }
    if (!found) return false;
  }
  return true;
}

NameGuess guess_name_encoding(std::span<const std::uint8_t> raw) {
  NameGuess g;
  if (is_ascii(raw) || is_valid_utf8(raw)) {
    g.utf8 = decode(raw, Codepage::utf8);
    g.exact = true;
    return g;
  }
  // Score Russian code pages by the share of high bytes that land on letters.
  std::size_t high = 0, cp866_letters = 0, cp1251_letters = 0;
  for (auto c : raw) {
    if (c < 0x80) continue;
    ++high;
    if (c <= 0xAF || (c >= 0xE0 && c <= 0xF1)) ++cp866_letters;
    if (c >= 0xC0 || c == 0xA8 || c == 0xB8) ++cp1251_letters;
  }
  const double s866 = static_cast<double>(cp866_letters) / static_cast<double>(high);
  const double s1251 = static_cast<double>(cp1251_letters) / static_cast<double>(high);
  // Lowercase Cyrillic dominates real names; in CP866 0xE0–0xEF/0xA0–0xAF,
  // in CP1251 0xE0–0xFF. Both tables can claim "all letters" for short names
  // (e.g. CP1251 "мир" = EC E8 F0 is "ьшЁ" in CP866). Natural words contain
  // vowels, so the decoding with more Russian vowels wins the tie.
  if (s866 >= 0.9 && s1251 >= 0.9) {
    auto vowels = [](const std::string& u) {
      // Ё/ё deliberately excluded: CP866 maps F0/F1 to them, which would let
      // byte runs like "ьшЁ" look as vowel-rich as real words.
      static constexpr std::u32string_view kV = U"аеиоуыэюяАЕИОУЫЭЮЯ";
      std::size_t n = 0;
      for (std::size_t i = 0; i < u.size();) {
        if (kV.find(next_cp(u, i)) != std::u32string_view::npos) ++n;
      }
      return n;
    };
    const auto d866 = decode(raw, Codepage::cp866);
    const auto d1251 = decode(raw, Codepage::cp1251);
    const bool pick866 = vowels(d866) >= vowels(d1251);
    g.codepage = pick866 ? Codepage::cp866 : Codepage::cp1251;
    g.utf8 = pick866 ? d866 : d1251;
    g.confidence = 0.6;
    return g;
  }
  if (s866 >= 0.9) {
    g.codepage = Codepage::cp866;
    g.confidence = 0.85;
  } else if (s1251 >= 0.9) {
    g.codepage = Codepage::cp1251;
    g.confidence = 0.8;
  } else {
    g.codepage = Codepage::cp437;  // PKWARE default when nothing else fits
    g.confidence = 0.4;
  }
  g.utf8 = decode(raw, g.codepage);
  return g;
}

std::string repair_cp437_mojibake(std::string_view utf8) {
  std::string bytes;
  if (!encode(utf8, Codepage::cp437, bytes)) return std::string(utf8);
  auto span = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
  if (is_ascii(span) || !is_valid_utf8(span)) return std::string(utf8);
  return bytes;
}

std::string to_base64(std::span<const std::uint8_t> b) {
  static constexpr char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((b.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < b.size(); i += 3) {
    std::uint32_t v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
    out += kTbl[v >> 18]; out += kTbl[(v >> 12) & 63]; out += kTbl[(v >> 6) & 63]; out += kTbl[v & 63];
  }
  if (i + 1 == b.size()) {
    std::uint32_t v = b[i] << 16;
    out += kTbl[v >> 18]; out += kTbl[(v >> 12) & 63]; out += "==";
  } else if (i + 2 == b.size()) {
    std::uint32_t v = (b[i] << 16) | (b[i + 1] << 8);
    out += kTbl[v >> 18]; out += kTbl[(v >> 12) & 63]; out += kTbl[(v >> 6) & 63]; out += '=';
  }
  return out;
}

}  // namespace am::text
