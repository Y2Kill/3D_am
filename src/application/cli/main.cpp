// am — command line interface (ТЗ 3.1.5). Uses the same core API as the GUI.
// M0 provides diagnostic verbs used by the spikes; real verbs (asset, library,
// import, lookup, dedup, jobs) arrive with their milestones (docs/plan/roadmap.md)
// and will move to CLI11 together with the vcpkg dependency set.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "am/storage/archive/volume_detect.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/signature.hpp"
#include "am/foundation/text_encoding.hpp"
#include "am/foundation/uuid.hpp"
#include "am/foundation/version.hpp"
#include "am/processing/formats/stl_probe.hpp"
#include "am/services/import/exclusion_rules.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW (excluded by WIN32_LEAN_AND_MEAN)
#endif

namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
  auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

int usage() {
  std::puts(
      "am " "— 3D/2D asset catalog (development build)\n"
      "usage:\n"
      "  am version\n"
      "  am uuid [--v4] [count]                new UUIDv7 ids (or UUIDv4 with --v4)\n"
      "  am probe <file>...                    content type (+ STL stats), read-only\n"
      "  am volumes <dir>                      multi-volume candidates among files in <dir>\n"
      "  am zip-path <root> <source-path>      asset-internal path mapping (ADR-0012)\n"
      "  am fix-name <text>                    repair CP437 mojibake (\"ΓÇô\" -> \"–\")\n"
      "  am exclude <entry-path> [file]        exclusion rule verdict for an entry\n");
  return 2;
}

int cmd_probe(const std::vector<std::string>& files) {
  int rc = 0;
  for (const auto& f : files) {
    const fs::path p = am::paths::native(fs::absolute(from_u8(f)));
    std::ifstream in(p, std::ios::binary);
    if (!in) { std::cout << f << ": cannot open\n"; rc = 1; continue; }
    std::vector<std::uint8_t> head(am::kSignatureProbeBytes * 4);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    const auto kind = am::detect_kind(head, ec ? std::nullopt : std::optional<std::uint64_t>(size));
    std::cout << f << ": " << am::to_string(kind) << "  size=" << size << "\n";
    if (kind == am::FileKind::stl_binary || kind == am::FileKind::stl_ascii) {
      auto s = am::formats::probe_stl_file(f);
      if (!s) { std::cout << "  stl error: " << am::to_log_string(s.error()) << "\n"; rc = 1; continue; }
      std::printf("  triangles=%llu bbox=[%.3f %.3f %.3f] area=%.3f volume=%.3f degenerate=%llu header=\"%s\"\n",
                  static_cast<unsigned long long>(s->triangles), s->max[0] - s->min[0], s->max[1] - s->min[1],
                  s->max[2] - s->min[2], s->area, s->volume, static_cast<unsigned long long>(s->degenerate),
                  s->header.c_str());
    }
  }
  return rc;
}

int cmd_volumes(const std::string& dir) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(am::paths::native(fs::absolute(from_u8(dir))), ec))
    if (e.is_regular_file()) names.push_back(u8(e.path().filename()));
  if (ec) { std::cout << "error: " << ec.message() << "\n"; return 1; }
  auto r = am::archive::group_volumes(names);
  for (const auto& v : r.volume_sets) {
    std::cout << "[" << am::archive::to_string(v.scheme) << "] " << v.base
              << (v.needs_header_confirmation ? "  (confirm by headers!)" : "") << "\n";
    for (const auto& m : v.members) std::cout << "    " << m << "\n";
    for (int gap : v.missing_indices) std::cout << "    MISSING #" << gap << "\n";
  }
  std::cout << r.singles.size() << " single file(s)\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  // Re-read arguments as UTF-16 so non-ASCII paths survive on Windows.
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  std::vector<std::string> args;
  for (int i = 1; i < wargc; ++i) args.push_back(u8(fs::path(wargv[i])));
  LocalFree(wargv);
  (void)argc; (void)argv;
#else
  std::vector<std::string> args(argv + 1, argv + argc);
#endif
  if (args.empty()) return usage();
  const std::string& verb = args[0];

  if (verb == "version") {
    std::cout << am::kAppName << " " << am::kAppVersion << " (asset format v" << am::kAssetFormatVersion << ")\n";
    return 0;
  }
  if (verb == "uuid") {
    const bool v4 = args.size() > 1 && args[1] == "--v4";
    const std::size_t count_index = v4 ? 2 : 1;
    int n = args.size() > count_index ? std::stoi(args[count_index]) : 1;
    for (int i = 0; i < n; ++i) std::cout << (v4 ? am::Uuid::v4() : am::Uuid::v7()).to_string() << "\n";
    return 0;
  }
  if (verb == "probe" && args.size() > 1) return cmd_probe({args.begin() + 1, args.end()});
  if (verb == "volumes" && args.size() == 2) return cmd_volumes(args[1]);
  if (verb == "zip-path" && args.size() == 3) {
    am::paths::ZipPathMapper m(args[1]);
    auto r = m.map(args[2]);
    std::cout << r.zip_path << "  [" << am::paths::to_string(r.reason) << "]\n";
    return 0;
  }
  if (verb == "fix-name" && args.size() == 2) {
    std::cout << am::text::repair_cp437_mojibake(args[1]) << "\n";
    return 0;
  }
  if (verb == "exclude" && args.size() >= 2) {
    am::import::EntryFacts facts;
    const std::string norm = am::paths::normalize_source_path(args[1]);
    facts.path = norm;
    if (args.size() == 3) {
      std::ifstream in(am::paths::native(fs::absolute(from_u8(args[2]))), std::ios::binary);
      std::vector<std::uint8_t> head(am::kSignatureProbeBytes);
      in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
      head.resize(static_cast<std::size_t>(in.gcount()));
      facts.kind = am::detect_kind(head);
    }
    auto m = am::import::ExclusionEngine{}.evaluate(facts);
    const char* conf = m.confidence == am::import::Confidence::certain ? "certain"
                     : m.confidence == am::import::Confidence::probable ? "probable" : "keep";
    std::cout << conf << (m.rule_id.empty() ? "" : "  " + m.rule_id + " — " + m.explanation) << "\n";
    return 0;
  }
  return usage();
}
