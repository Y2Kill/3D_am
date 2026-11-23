#include "am/services/import/exclusion_rules.hpp"

#include <algorithm>

namespace am::import {
namespace {

std::string_view basename(std::string_view p) {
  auto pos = p.rfind('/');
  return pos == std::string_view::npos ? p : p.substr(pos + 1);
}

bool has_component(std::string_view p, std::string_view comp) {
  std::size_t start = 0;
  while (start <= p.size()) {
    auto end = p.find('/', start);
    if (end == std::string_view::npos) end = p.size();
    if (p.substr(start, end - start) == comp) return true;
    start = end + 1;
  }
  return false;
}

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           auto l = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
           return l(x) == l(y);
         });
}

RuleMatch appledouble(const EntryFacts& e) {
  const auto name = basename(e.path);
  const bool in_macosx = has_component(e.path, "__MACOSX");
  const bool dot_underscore = name.size() > 2 && name.rfind("._", 0) == 0;
  const bool sig = e.kind && *e.kind == FileKind::apple_double;
  if ((in_macosx || dot_underscore) && sig)
    return {"macos.appledouble", Confidence::certain, "AppleDouble resource fork (macOS)"};
  if (sig) return {"macos.appledouble", Confidence::certain, "AppleDouble signature 00 05 16 07"};
  if (in_macosx && dot_underscore)
    return {"macos.appledouble", Confidence::probable, "__MACOSX/._* without readable header"};
  if (in_macosx) return {"macos.macosx_dir", Confidence::probable, "file inside __MACOSX/"};
  return {};
}

RuleMatch ds_store(const EntryFacts& e) {
  if (!iequals(basename(e.path), ".DS_Store")) return {};
  if (e.kind && *e.kind == FileKind::ds_store)
    return {"macos.ds_store", Confidence::certain, "Finder metadata (.DS_Store)"};
  return {"macos.ds_store", Confidence::probable, ".DS_Store by name"};
}

RuleMatch thumbs_db(const EntryFacts& e) {
  const auto n = basename(e.path);
  if (!iequals(n, "Thumbs.db") && !iequals(n, "ehthumbs.db") && !iequals(n, "ehthumbs_vista.db")) return {};
  if (e.kind && *e.kind == FileKind::ole_compound)
    return {"windows.thumbs_db", Confidence::certain, "Windows thumbnail cache"};
  return {"windows.thumbs_db", Confidence::probable, "Thumbs.db by name"};
}

RuleMatch desktop_ini(const EntryFacts& e) {
  if (!iequals(basename(e.path), "desktop.ini")) return {};
  // Plain-text INI, no signature: small size is the structural check.
  if (e.size && *e.size < 4096)
    return {"windows.desktop_ini", Confidence::certain, "Explorer folder settings"};
  return {"windows.desktop_ini", Confidence::probable, "desktop.ini by name"};
}

RuleMatch icon_r(const EntryFacts& e) {
  if (basename(e.path) == "Icon\r") return {"os.icon_r", Confidence::certain, "macOS custom folder icon"};
  return {};
}

int rank(Confidence c) { return c == Confidence::certain ? 2 : c == Confidence::probable ? 1 : 0; }

}  // namespace

ExclusionEngine::ExclusionEngine()
    : rules_{{"macos.appledouble", &appledouble},
             {"macos.ds_store", &ds_store},
             {"windows.thumbs_db", &thumbs_db},
             {"windows.desktop_ini", &desktop_ini},
             {"os.icon_r", &icon_r}} {}

RuleMatch ExclusionEngine::evaluate(const EntryFacts& entry) const {
  RuleMatch best;
  for (const auto& r : rules_) {
    RuleMatch m = r.fn(entry);
    if (rank(m.confidence) > rank(best.confidence)) best = std::move(m);
  }
  return best;
}

RuleMatch ExclusionEngine::evaluate(const EntryFacts& entry, std::span<const std::string> enabled_rules) const {
  RuleMatch best;
  for (const auto& rule : rules_) {
    if (std::find(enabled_rules.begin(), enabled_rules.end(), rule.id) == enabled_rules.end()) continue;
    auto match = rule.fn(entry);
    if (rank(match.confidence) > rank(best.confidence)) best = std::move(match);
  }
  return best;
}

std::vector<std::string> ExclusionEngine::rule_ids() const {
  std::vector<std::string> ids;
  for (const auto& r : rules_) ids.push_back(r.id);
  return ids;
}

}  // namespace am::import
