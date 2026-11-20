#include "plan.hpp"
#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>

namespace {
std::uint64_t number(std::string_view s) {
  std::uint64_t n = 0; const auto result = std::from_chars(s.data(),s.data()+s.size(),n);
  if (result.ec != std::errc{} || result.ptr != s.data()+s.size()) throw std::runtime_error("Invalid unsigned integer");
  return n;
}
std::uint64_t fraction(std::string s) {
  const auto dot = s.find('.'); const auto whole = dot == std::string::npos ? s : s.substr(0,dot);
  auto decimal = dot == std::string::npos ? std::string{} : s.substr(dot+1);
  if (decimal.size() > 6) throw std::runtime_error("Trash fraction supports at most six decimal places");
  while (decimal.size() < 6) decimal += '0';
  const auto ppm = number(whole)*1000000+number(decimal);
  if (ppm > 1000000 || number(whole) > 1) throw std::runtime_error("Trash fraction must be between zero and one");
  return ppm;
}
}
int main(int argc, char** argv) {
  using namespace perf;
  try {
    Parameters p; fs::path out; bool plan_only = false, json = false, assume = false;
    std::uint64_t stop_after = std::numeric_limits<std::uint64_t>::max();
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--plan-only") plan_only = true;
      else if (arg == "--json") json = true;
      else if (arg == "--assume-capabilities") assume = true;
      else {
        if (++i >= argc) throw std::runtime_error("Missing argument value");
        const std::string s = argv[i];
        if (arg == "--out") out = fs::path(std::u8string(s.begin(),s.end()));
        else if (arg == "--count") p.count = number(s);
        else if (arg == "--seed") p.seed = number(s);
        else if (arg == "--content-scale") p.scale = number(s);
        else if (arg == "--trash-fraction") p.trash_ppm = fraction(s);
        else if (arg == "--stop-after") stop_after = number(s);
        else throw std::runtime_error("Unknown option");
      }
    }
    if (out.empty() || p.count == 0 || p.count > 250000 || p.scale == 0 ||
        p.scale > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())/16384 ||
        p.scale > std::numeric_limits<std::size_t>::max()/16384)
      throw std::runtime_error("Usage: am_gen_library --out DIR --count 1..250000 [--seed S] [--trash-fraction 0..1] [--content-scale K] [--plan-only] [--json] [--assume-capabilities] [--stop-after K]");
    const auto started = std::chrono::steady_clock::now();
    auto descriptor = out; descriptor += ".gen.json";
    if (!plan_only) {
      // Check before opening the library: even owner/session records must not change on mismatch.
      if (fs::exists(descriptor)) {
        std::ifstream f(am::paths::native(descriptor),std::ios::binary); Json previous; f >> previous;
        if (previous != p.json()) throw std::runtime_error("Generation parameters differ from the existing descriptor; library unchanged");
      } else {
        if (fs::exists(out) && !fs::is_empty(out)) throw std::runtime_error("Existing nonempty directory has no generation descriptor; library unchanged");
        if (!out.parent_path().empty()) fs::create_directories(out.parent_path());
        auto temp = descriptor; temp += ".tmp"; write(temp,p.json().dump(2)+"\n"); fs::rename(temp,descriptor);
      }
      if (!fs::exists(out/"am-library.toml")) take(am::library::Library::init(out));
    }
    std::optional<am::catalog::Catalog> catalog;
    std::map<std::string,bool> existing;
    if (!plan_only) {
      catalog.emplace(take(am::catalog::Catalog::open(out,options(assume))));
      if (take(catalog->indexer().rebuild_required())) take(catalog->indexer().rebuild());
      for (const auto& row : take(catalog->indexer().list(true))) existing.emplace(row.asset_id,row.in_trash);
    }
    auto hash = take(am::asset_zip::Sha256::create()); Json asset_hashes = Json::array();
    std::uint64_t files = 0, trash = 0, created = 0, skipped = 0, processed = 0;
    for (std::uint64_t i = 0; i < p.count; ++i) {
      if (!plan_only && processed >= stop_after) break;
      const auto a = asset(p,i); const auto line = a.canonical().dump()+"\n"; update(hash,line);
      files += a.files.size(); trash += a.trash ? 1 : 0;
      if (plan_only) { asset_hashes.push_back(sha(line)); continue; }
      const auto found = existing.find(a.id);
      if (found == existing.end()) { auto scratch = out; scratch += ".sources"; create(*catalog,a,scratch); ++created; }
      else ++skipped;
      if (a.trash && (found == existing.end() || !found->second)) {
        const auto result = take(catalog->trash({catalog->library().config().library_id,a.id}));
        if (!result.warnings.empty()) throw std::runtime_error("Catalog trash returned index warnings");
      }
      ++processed;
    }
    catalog.reset();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    Json report{{"schema","am.perf.generator.v1"},{"parameters",p.json()},{"plan_only",plan_only},
      {"assume_capabilities",assume},{"assets",plan_only ? p.count : processed},{"files",files},{"trash",trash},
      {"created",created},{"skipped",skipped},{"generation_seconds",seconds},
      {"assets_per_second",seconds > 0 ? static_cast<double>(created)/seconds : 0.0}};
    if (plan_only || processed == p.count) report["plan_sha256"] = digest(hash);
    else report["plan_sha256"] = nullptr;
    if (plan_only) report["asset_sha256"] = std::move(asset_hashes);
    if (json) std::cout << report.dump() << '\n';
    else std::cout << "assume_capabilities=" << (assume ? "true (synthetic override)" : "false") << '\n' << report.dump(2) << '\n';
    return 0;
  } catch (const std::exception& e) { std::cerr << "am_gen_library: " << e.what() << '\n'; return 1; }
}
