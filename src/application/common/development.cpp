#include "am/application/development.hpp"
#include "am/storage/archive/volume_detect.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/signature.hpp"
#include "am/foundation/text_encoding.hpp"
#include "am/foundation/uuid.hpp"
#include "am/foundation/version.hpp"
#include "am/processing/formats/stl_probe.hpp"
#include "am/services/import/exclusion_rules.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace am::application {
namespace {
namespace fs = std::filesystem;
std::string u8(const fs::path& p) {
  auto s = p.u8string();
  return {s.begin(), s.end()};
}
fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
}  // namespace
Result<DevelopmentOutput> development_command(std::string_view verb, const std::vector<std::string>& args) {
  try {
    std::ostringstream out;
    int code = 0;
    if (verb == "version")
      out << kAppName << " " << kAppVersion << " (asset format v" << kAssetFormatVersion << ")\n";
    else if (verb == "uuid") {
      bool v4 = !args.empty() && args[0] == "--v4";
      auto index = v4 ? 1u : 0u;
      auto count = args.size() > index ? std::stoi(args[index]) : 1;
      for (int i = 0; i < count; ++i) out << (v4 ? Uuid::v4() : Uuid::v7()).to_string() << '\n';
    } else if (verb == "zip-path") {
      auto mapper = paths::ZipPathMapper::create(args.at(0));
      if (!mapper) {
        out << "error: " << to_log_string(mapper.error()) << '\n';
        code = 1;
      } else {
        auto mapped = mapper->map(args.at(1));
        out << mapped.zip_path << "  [" << paths::to_string(mapped.reason) << "]\n";
      }
    } else if (verb == "fix-name")
      out << text::repair_cp437_mojibake(args.at(0)) << '\n';
    else if (verb == "exclude") {
      import::EntryFacts facts;
      auto norm = paths::normalize_source_path(args.at(0));
      facts.path = norm;
      if (args.size() == 2) {
        std::ifstream in(paths::native(fs::absolute(from_u8(args[1]))), std::ios::binary);
        std::vector<std::uint8_t> head(kSignatureProbeBytes);
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(in.gcount()));
        facts.kind = detect_kind(head);
      }
      auto match = import::ExclusionEngine{}.evaluate(facts);
      const char* confidence = match.confidence == import::Confidence::certain    ? "certain"
                               : match.confidence == import::Confidence::probable ? "probable"
                                                                                  : "keep";
      out << confidence << (match.rule_id.empty() ? "" : "  " + match.rule_id + " — " + match.explanation) << '\n';
    } else if (verb == "volumes") {
      std::vector<std::string> names;
      std::error_code ec;
      for (const auto& entry : fs::directory_iterator(paths::native(fs::absolute(from_u8(args.at(0)))), ec))
        if (entry.is_regular_file()) names.push_back(u8(entry.path().filename()));
      if (ec) {
        out << "error: " << ec.message() << '\n';
        code = 1;
      } else {
        auto groups = archive::group_volumes(names);
        for (const auto& group : groups.volume_sets) {
          out << "[" << archive::to_string(group.scheme) << "] " << group.base
              << (group.needs_header_confirmation ? "  (confirm by headers!)" : "") << '\n';
          for (const auto& member : group.members) out << "    " << member << '\n';
          for (int gap : group.missing_indices) out << "    MISSING #" << gap << '\n';
        }
        out << groups.singles.size() << " single file(s)\n";
      }
    } else if (verb == "probe") {
      for (const auto& file : args) {
        auto path = paths::native(fs::absolute(from_u8(file)));
        std::ifstream in(path, std::ios::binary);
        if (!in) {
          out << file << ": cannot open\n";
          code = 1;
          continue;
        }
        std::vector<std::uint8_t> head(kSignatureProbeBytes * 4);
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<std::size_t>(in.gcount()));
        std::error_code ec;
        auto size = fs::file_size(path, ec);
        auto kind = detect_kind(head, ec ? std::nullopt : std::optional<std::uint64_t>(size));
        out << file << ": " << to_string(kind) << "  size=" << size << '\n';
        if (kind == FileKind::stl_binary || kind == FileKind::stl_ascii) {
          auto stats = formats::probe_stl_file(file);
          if (!stats) {
            out << "  stl error: " << to_log_string(stats.error()) << '\n';
            code = 1;
            continue;
          }
          out << "  triangles=" << stats->triangles << " bbox=[" << std::fixed << std::setprecision(3)
              << stats->max[0] - stats->min[0] << ' ' << stats->max[1] - stats->min[1] << ' '
              << stats->max[2] - stats->min[2] << "] area=" << stats->area << " volume=" << stats->volume
              << " degenerate=" << stats->degenerate << " header=\"" << stats->header << "\"\n";
        }
      }
    } else
      return Error::make(Errc::invalid_argument, "Unknown development command").id("am.app.invalid_command");
    return DevelopmentOutput{code, out.str()};
  } catch (const std::bad_alloc&) {
    return Error::make(Errc::resource_exhausted, "Diagnostic allocation failed").id("am.app.out_of_memory");
  } catch (const std::exception& e) {
    return Error::make(Errc::io_error, "Diagnostic operation failed", e.what()).id("am.app.io_error");
  } catch (...) {
    return Error::make(Errc::internal, "Diagnostic operation failed").id("am.app.internal");
  }
}
}  // namespace am::application
