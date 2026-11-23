#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>

#include "am/application/import_service.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/storage/asset_zip/sha256.hpp"
#include "log_test_sink.hpp"
#include "zip_builder.hpp"
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace imp = am::import;
namespace app = am::application;
using J = imp::PlanJson;
template <class T>
T take(am::Result<T> value) {
  if (!value) throw std::runtime_error(am::to_log_string(value.error()));
  return std::move(*value);
}
void take(am::Result<void> value) {
  if (!value) throw std::runtime_error(am::to_log_string(value.error()));
}
void write(const fs::path& file, std::string_view bytes) {
  fs::create_directories(am::paths::native(file.parent_path()));
  std::ofstream out(am::paths::native(file), std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out) throw std::runtime_error("Fixture write failed");
}
std::string bytes(const fs::path& file) {
  std::ifstream in(am::paths::native(file), std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
std::string hash(std::string_view data) {
  auto h = take(am::asset_zip::Sha256::create());
  take(h.update(std::as_bytes(std::span(data.data(), data.size()))));
  const auto digest = take(h.finish());
  std::string out;
  constexpr char digits[] = "0123456789abcdef";
  for (auto byte : digest) {
    const auto b = std::to_integer<unsigned>(byte);
    out += digits[b >> 4];
    out += digits[b & 15];
  }
  return out;
}
auto snapshot(const fs::path& root) {
  std::map<fs::path, std::tuple<std::string, std::uintmax_t, fs::file_time_type, std::string>> out;
  out.emplace(fs::path{}, std::tuple("directory", 0, fs::last_write_time(am::paths::native(root)), std::string{}));
  for (const auto& entry : fs::recursive_directory_iterator(am::paths::native(root))) {
    const auto status = entry.symlink_status();
    const auto path = entry.path().lexically_relative(am::paths::native(root));
    if (fs::is_regular_file(status))
      out.emplace(path, std::tuple("file", fs::file_size(entry.path()), fs::last_write_time(entry.path()),
                                   hash(bytes(entry.path()))));
    else if (fs::is_directory(status))
      out.emplace(path, std::tuple("directory", 0, fs::last_write_time(entry.path()), std::string{}));
  }
  return out;
}
auto library_snapshot(const fs::path& root) {
  auto result = snapshot(root);
  // Match T-027's accepted check: database/ZIP/config bytes and mtimes.
  // SQLite's existing read-only WAL mode may create its transient sidecars.
  std::erase_if(result, [](const auto& item) {
    const auto& p = item.first;
    return p.extension() != ".sqlite" && p.extension() != ".zip" && p.filename() != "am-library.toml";
  });
  return result;
}
fs::path fresh() {
  auto root = fs::path(AM_IMPORT_SCAN_ROOT) / am::Uuid::v4().to_string();
  fs::create_directories(root);
  return root;
}
imp::ScanRequest request(const std::vector<fs::path>& roots, std::string_view profile = "default") {
  imp::ScanRequest r;
  r.roots = roots;
  r.profile = take(app::load_import_profile(profile));
  r.hooks.clock = [] { return std::chrono::system_clock::time_point(std::chrono::seconds(1795392000)); };
  return r;
}
J run(const imp::ScanRequest& r) {
  auto plan = take(imp::scan(r));
  auto encoded = imp::serialize_plan(plan);
  auto parsed = take(imp::parse_plan(encoded));
  fs::create_directories(AM_IMPORT_PLAN_CASES);
  write(fs::path(AM_IMPORT_PLAN_CASES) / (am::Uuid::v4().to_string() + ".json"), encoded);
  return parsed.value;
}
const J& node(const J& plan, std::string_view filename) {
  for (const auto& value : plan["nodes"])
    if (value["name"].get<std::string>() == filename) return value;
  throw std::runtime_error("Node not found");
}
const J& entry(const J& plan, std::string_view filename) {
  const auto id = node(plan, filename)["id"];
  for (const auto& c : plan["candidates"])
    for (const auto& value : c["entries"])
      if (value["node"] == id) return value;
  throw std::runtime_error("Entry not found");
}
bool reason(const J& candidate, std::string_view value) {
  const auto& list = candidate["reasons"];
  return std::find(list.begin(), list.end(), std::string(value)) != list.end();
}
void zip(const fs::path& file, const std::vector<zip_test::Entry>& entries) {
  const auto data = zip_test::build(entries).bytes;
  write(file, {reinterpret_cast<const char*>(data.data()), data.size()});
}
std::error_code directory_link(const fs::path& target, const fs::path& link) {
  std::error_code error;
  fs::create_directory_symlink(target, link, error);
#ifdef _WIN32
  if (error) {
    const auto command = L"cmd.exe /d /v:off /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\"";
    std::wstring mutable_command = command;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                       &startup, &process)) {
      WaitForSingleObject(process.hProcess, INFINITE);
      DWORD code = 1;
      GetExitCodeProcess(process.hProcess, &code);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      if (code == 0) error.clear();
    }
  }
#endif
  return error;
}
am::library::OpenOptions capabilities() {
  am::library::OpenOptions out;
  out.capabilities.emplace();
  out.capabilities->mode = am::repository::StorageMode::full;
  return out;
}
}  // namespace
TEST(ImportScan, SingleFilesMultipleRootsSingleAndMixedInputs) {
  RecordProperty("req", "SC-1|SC-2|SC-8");
  const auto root = fresh();
  write(root / "one.bin", "one");
  write(root / "two.bin", "two");
  auto one = run(request({root / "one.bin"}));
  ASSERT_EQ(one["candidates"].size(), 1u);
  EXPECT_EQ(one["nodes"][0]["size"], "3");
  auto r = request({root / "one.bin", root / "two.bin"});
  EXPECT_EQ(run(r)["candidates"].size(), 2u);
  r.single_candidate = true;
  auto combined = run(r);
  ASSERT_EQ(combined["candidates"].size(), 1u);
  EXPECT_EQ(combined["candidates"][0]["entries"].size(), 2u);
  write(root / "folder" / "sub" / "part.stl", "solid part facet");
  auto mixed = run(request({root / "folder", root / "one.bin"}));
  ASSERT_EQ(mixed["candidates"].size(), 2u);
  EXPECT_EQ(mixed["candidates"][0]["directories"].size(), 2u);
}
TEST(ImportScan, EmptyDirectoriesZeroFilesAndUnknownTypesAreRetained) {
  RecordProperty("req", "SC-3|SC-11|SC-12|SC-16");
  const auto root = fresh();
  fs::create_directories(root / "empty");
  write(root / "zero", "");
  write(root / "unknown.opaque", "opaque");
  auto p = run(request({root}));
  ASSERT_EQ(p["candidates"].size(), 1u);
  EXPECT_TRUE(node(p, "empty")["empty"].get<bool>());
  bool directory_object = false;
  for (const auto& input : p["candidates"][0]["inputs"])
    for (const auto& object : input["objects"])
      if (object.contains("node") && object["node"] == node(p, "empty")["id"]) directory_object = true;
  EXPECT_TRUE(directory_object);
  EXPECT_EQ(node(p, "zero")["size"], "0");
  EXPECT_EQ(node(p, "unknown.opaque")["detected"], "unknown");
  EXPECT_EQ(entry(p, "zero")["decision"], "include");
  auto empty = run(request({root / "empty"}));
  EXPECT_TRUE(empty["nodes"][0]["empty"].get<bool>());
}
TEST(ImportScan, SignatureWinsOverExtensionAndSpecializedZipIsAFile) {
  RecordProperty("req", "SC-6|SC-10|K-8");
  const auto root = fresh();
  write(root / "fake.zip", std::string("Rar!\x1a\x07\0", 7));
  zip_test::Entry project;
  project.name = "3D/3dmodel.model";
  project.data = "<model/>";
  zip(root / "project.3mf", {project});
  auto p = run(request({root}));
  EXPECT_EQ(node(p, "fake.zip")["detected"], "archive.rar4");
  EXPECT_EQ(node(p, "project.3mf")["detected"], "project.3mf");
  bool file = false;
  for (const auto& input : p["candidates"][0]["inputs"])
    if (input["kind"] == "directory")
      for (const auto& object : input["objects"]) file = file || object["node"] == node(p, "project.3mf")["id"];
  EXPECT_TRUE(file);
}
TEST(ImportScan, StableByteOrderAndDeterministicSerialization) {
  RecordProperty("req", "SC-3");
  const auto root = fresh();
  write(root / "a" / "z", "one");
  write(root / "a.bin", "two");
  write(root / "Z", "three");
  auto r = request({root});
  auto first = take(imp::scan(r)), second = take(imp::scan(r));
  EXPECT_EQ(imp::serialize_plan(first), imp::serialize_plan(second));
  std::vector<std::string> paths;
  for (const auto& n : first.value["nodes"]) paths.push_back(n["path"].get<std::string>());
  EXPECT_TRUE(std::is_sorted(paths.begin(), paths.end()));
}
TEST(ImportScan, SourceTreeIsUnchangedAndOpensAreReadOnly) {
  RecordProperty("req", "SC-3|K-1");
  const auto root = fresh();
  write(root / "sub" / "data", std::string(100000, 'x'));
  const auto before = snapshot(root);
  auto r = request({root});
  r.hash_inputs = true;
  unsigned opens = 0;
  r.hooks.on_open = [&](const auto&, bool writable) {
    EXPECT_FALSE(writable);
    ++opens;
  };
  auto p = run(r);
  EXPECT_EQ(snapshot(root), before);
  EXPECT_GT(opens, 0u);
  EXPECT_EQ(node(p, "data")["sha256"], hash(std::string(100000, 'x')));
}
TEST(ImportScan, LinksAndCyclesAreRecordedWithoutFollowingButExplicitRootResolves) {
  RecordProperty("req", "SC-3|K-1");
  const auto root = fresh(), outside = fresh();
  write(outside / "outside-canary", "private");
  auto error = directory_link(outside, root / "link");
  if (error) {
    GTEST_SKIP() << "Source directory links/junctions cannot be created in this environment: " << error.message();
  }
  error = directory_link(root, root / "cycle");
  ASSERT_FALSE(error) << error.message();
  auto p = run(request({root}));
  EXPECT_EQ(node(p, "link")["kind"], "symlink");
  EXPECT_EQ(entry(p, "link")["decision"], "skip");
  EXPECT_EQ(entry(p, "cycle")["decision"], "skip");
  for (const auto& n : p["nodes"]) EXPECT_NE(n["name"], "outside-canary");
  auto direct = run(request({root / "link"}));
  EXPECT_EQ(node(direct, "outside-canary")["kind"], "file");
  auto rejected = app::scan_import(request({outside}), root / "link" / "forbidden.json");
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().message_id, "am.import.output_inside_source");
  EXPECT_FALSE(fs::exists(outside / "forbidden.json"));
}
TEST(ImportScan, RawInvalidNamesArePreservedWhenTheFilesystemRetainsThem) {
  RecordProperty("req", "SC-3|K-8");
  const auto root = fresh();
  fs::path malformed;
#ifdef _WIN32
  malformed = root / fs::path(std::wstring(L"bad") + static_cast<wchar_t>(0xd800) + L".bin");
#else
  malformed = root / fs::path(std::string("bad\xff.bin"));
#endif
  write(malformed, "raw");
  bool native_name_retained = false;
  for (const auto& entry : fs::directory_iterator(am::paths::native(root)))
    native_name_retained = native_name_retained || entry.path().filename().native() == malformed.filename().native();
  if (!native_name_retained) {
    GTEST_SKIP() << "The filesystem normalizes the invalid native filename before enumeration; this mount cannot "
                    "provide the raw-name fixture";
  }
  auto p = run(request({root}));
  bool raw = false;
  for (const auto& n : p["nodes"])
    if (n.contains("name_raw")) {
      raw = true;
      EXPECT_NE(n["name"].get<std::string>().find("\xef\xbf\xbd"), std::string::npos);
      EXPECT_TRUE(n.contains("path_raw"));
#ifdef _WIN32
      EXPECT_EQ(n["name_raw"], "62006100640000d82e00620069006e00");
      EXPECT_EQ(n["raw_encoding"], "utf-16le");
#else
      EXPECT_EQ(n["name_raw"], "626164ff2e62696e");
      EXPECT_EQ(n["raw_encoding"], "posix");
#endif
      EXPECT_EQ(n["path_raw"],n["name_raw"]);
    }
  EXPECT_TRUE(raw);
}
TEST(ImportScan, LongPathsArePreserved) {
  RecordProperty("req", "SC-3|K-8");
  const auto root = fresh();
  auto long_path = root;
  for (unsigned i = 0; i < 20; ++i) long_path /= std::string(20, 'p');
  write(long_path / "long.bin", "long");
  auto p = run(request({root}));
  EXPECT_EQ(node(p, "long.bin")["size"], "4");
}
TEST(ImportScan, CaseCollisionIsAWarningAndNeverARename) {
  RecordProperty("req", "SC-2|K-8");
  const auto root = fresh();
  write(root / "left" / "A.stl", "one");
  write(root / "right" / "a.stl", "two");
  auto r = request({root / "left" / "A.stl", root / "right" / "a.stl"});
  r.single_candidate = true;
  auto p = run(r);
  ASSERT_EQ(p["candidates"].size(), 1u);
  EXPECT_EQ(p["candidates"][0]["warnings"], J::array({"case_collision"}));
}
TEST(ImportScan, StatOpenHeaderAndDisappearanceFailuresAreLocal) {
  RecordProperty("req", "SC-13");
  for (auto failure : {imp::ScanIo::stat, imp::ScanIo::open, imp::ScanIo::header}) {
    const auto root = fresh();
    write(root / "bad.bin", "bad");
    write(root / "good.bin", "good");
    auto r = request({root});
    r.hooks.before_io = [&](const auto& path, imp::ScanIo operation) -> am::Result<void> {
      if (path.filename() == "bad.bin" && operation == failure)
        return am::Error::make(am::Errc::io_error, "Injected failure").id("am.import.node_unavailable");
      return {};
    };
    auto p = run(r);
    EXPECT_EQ(node(p, "bad.bin")["status"], "unavailable");
    EXPECT_EQ(entry(p, "bad.bin")["decision"], "ask");
    EXPECT_EQ(node(p, "good.bin")["status"], "available");
    EXPECT_TRUE(reason(p["candidates"][0], "has_unavailable"));
  }
  const auto root = fresh();
  write(root / "vanished", "gone");
  write(root / "good", "good");
  auto r = request({root});
  r.hooks.before_io = [&](const auto& path, imp::ScanIo operation) -> am::Result<void> {
    if (path.filename() == "vanished" && operation == imp::ScanIo::stat) fs::remove(path);
    return {};
  };
  auto p = run(r);
  EXPECT_EQ(node(p, "vanished")["status"], "unavailable");
}
TEST(ImportScan, UnavailableRootCancellationAndLimitAreExplicit) {
  RecordProperty("req", "SC-13");
  const auto root = fresh();
  EXPECT_FALSE(imp::scan(request({root / "missing"})));
  for (unsigned i = 0; i < 8; ++i) write(root / ("file" + std::to_string(i)), "payload");
  auto r = request({root});
  bool cancelled = false;
  r.cancelled = [&] { return cancelled; };
  r.on_progress = [&](const auto& p) {
    if (p.nodes >= 3) cancelled = true;
  };
  auto result = imp::scan(r);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code, am::Errc::cancelled);
  r = request({root});
  r.profile.settings["limits"]["max_entries"] = 3;
  auto p = run(r);
  EXPECT_EQ(p["nodes"].size(), 3u);
  EXPECT_EQ(p["scan"]["completeness"], "incomplete");
  EXPECT_EQ(p["scan"]["limits_hit"], J::array({"max_entries"}));
}
TEST(ImportScan, ExclusionsAreProfileControlledAndDuplicatesStayIncluded) {
  RecordProperty("req", "SC-12|K-8");
  const auto root = fresh();
  write(root / "__MACOSX" / "._part.3mf", std::string("\0\5\x16\7", 4));
  write(root / "._x.3mf", "ordinary");
  write(root / ".DS_Store", std::string("\0\0\0\1Bud1", 8));
  write(root / "Thumbs.db", std::string("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8));
  write(root / "a" / "duplicate", "same");
  write(root / "b" / "duplicate", "same");
  auto r = request({root});
  auto p = run(r);
  EXPECT_EQ(entry(p, "._part.3mf")["decision"], "exclude");
  EXPECT_EQ(entry(p, "._x.3mf")["decision"], "include");
  EXPECT_EQ(entry(p, ".DS_Store")["decision"], "exclude");
  EXPECT_EQ(entry(p, "Thumbs.db")["decision"], "exclude");
  unsigned duplicates = 0;
  for (const auto& e : p["candidates"][0]["entries"])
    if (e["decision"] == "include") ++duplicates;
  EXPECT_GE(duplicates, 3u);
  r.profile.settings["exclusions"]["rules"] = J::array();
  p = run(r);
  EXPECT_EQ(entry(p, "._part.3mf")["decision"], "include");
  r.profile.settings["exclusions"]["rules"] = J::array({"unknown"});
  auto invalid = imp::scan(r);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().message_id, "am.import.profile_invalid");
}
TEST(ImportScan, VolumesHaveMissingIndicesRemainUnconfirmedAndStayTogetherAcrossRoots) {
  RecordProperty("req", "SC-6|SC-13");
  const auto root = fresh();
  for (const auto* name : {"X.part1.rar", "X.part2.rar", "X.part4.rar", "Y.7z.001", "Y.7z.002", "Z.rar", "Z (2).rar"})
    write(root / name, "opaque");
  auto p = run(request({root}));
  ASSERT_EQ(p["candidates"][0]["volume_sets"].size(), 3u);
  bool missing = false;
  for (const auto& v : p["candidates"][0]["volume_sets"]) {
    EXPECT_FALSE(v["confirmed"].get<bool>());
    if (v["scheme"] == "rar_part") {
      EXPECT_EQ(v["missing_indices"], J::array({3}));
      missing = true;
    }
  }
  EXPECT_TRUE(missing);
  auto separate = run(request({root / "X.part1.rar", root / "X.part2.rar"}));
  ASSERT_EQ(separate["candidates"].size(), 1u);
  EXPECT_EQ(separate["candidates"][0]["volume_sets"][0]["members"].size(), 2u);
  EXPECT_TRUE(reason(separate["candidates"][0], "volume_set_kept_together"));
}
TEST(ImportScan, TelegramContextDepthLooseInputsAndUrlAdvertisement) {
  RecordProperty("req", "SC-3|SC-5|SC-8|SC-16");
  const auto root = fresh();
  write(root / "Channel" / "Model" / "part.bin", "payload");
  write(root / "Channel" / "loose.bin", "loose");
  write(root / "Channel" / "Bundle.part1.rar", "volume one");
  write(root / "Channel" / "Bundle.part2.rar", "volume two");
  write(root / "Channel" / "Model" / "safe.url", "[InternetShortcut]\r\nURL=https://example.invalid/model\r\n");
  write(root / "Channel" / "Model" / "ad.url", "[InternetShortcut]\nURL=https://example.invalid/FREEdownload\n");
  auto p = run(request({root}, "telegram"));
  ASSERT_EQ(p["candidates"].size(), 3u);
  bool model = false, loose = false, ad = false, safe = false, bundle = false;
  for (const auto& c : p["candidates"]) {
    if (c["base_path"] == "Channel/Model") {
      model = true;
      EXPECT_EQ(c["context"]["sources"][0]["name"], "Channel");
      EXPECT_FALSE(c["context"]["sources"][0]["confirmed"].get<bool>());
      for (const auto& link : c["suggested_links"]) {
        EXPECT_FALSE(link["ad"].get<bool>());
        safe = true;
      }
      for (const auto& link : c["filtered_links"]) ad = ad || link["ad"].get<bool>();
    }
    if (c["confidence"] == "low") loose = true;
    if (!c["volume_sets"].empty()) {
      bundle = true;
      EXPECT_EQ(c["volume_sets"][0]["members"].size(), 2u);
      EXPECT_TRUE(reason(c, "volume_set_kept_together"));
    }
  }
  EXPECT_TRUE(model);
  EXPECT_TRUE(loose);
  EXPECT_TRUE(ad);
  EXPECT_TRUE(safe);
  EXPECT_TRUE(bundle);
  EXPECT_EQ(p["profile"]["not_applied"], J::array({"nested", "previews", "groups"}));
}
TEST(ImportScan, ReadyAssetsAreSeparatedEvenWithSingleAndInvalidAssetsAsk) {
  RecordProperty("req", "SC-14|SC-3|SC-2");
  const auto root = fresh();
  fs::copy_file(fs::path(AM_GOLDEN_FIXTURES) / "pos-minimal" / "asset.zip", root / "ready.zip");
  fs::copy_file(fs::path(AM_GOLDEN_FIXTURES) / "neg-v2-dimension" / "asset.zip", root / "bad.zip");
  write(root / "part.bin", "payload");
  zip_test::Entry ordinary;
  ordinary.name = "part.bin";
  zip(root / "ordinary.zip", {ordinary});
  for (bool single : {false, true}) {
    auto r = request({root});
    r.single_candidate = single;
    auto p = run(r);
    unsigned ready = 0, invalid = 0, fresh_candidates = 0;
    for (const auto& c : p["candidates"]) {
      if (c["kind"] == "ready_asset") {
        ++ready;
        EXPECT_EQ(c["inputs"].size(), 1u);
        if (reason(c, "ready_asset_invalid")) {
          ++invalid;
          EXPECT_EQ(c["action"], "ask");
        } else {
          EXPECT_TRUE(c.contains("asset_id"));
          EXPECT_TRUE(c.contains("revision"));
        }
      } else {
        ++fresh_candidates;
        EXPECT_TRUE(reason(c, "ready_asset_separated"));
      }
    }
    EXPECT_EQ(ready, 2u);
    EXPECT_EQ(invalid, 1u);
    EXPECT_EQ(fresh_candidates, 1u);
  }
  auto r = request({root / "ready.zip", root / "bad.zip"});
  r.single_candidate = true;
  auto p = run(r);
  ASSERT_EQ(p["candidates"].size(), 2u);
  for (const auto& c : p["candidates"]) EXPECT_EQ(c["kind"], "ready_asset");
}
TEST(ImportScan, ProfilesAreEmbeddedStrictAndKeepTheirOriginalByteHash) {
  RecordProperty("req", "SC-3");
  const auto root = fresh();
  auto profile = take(app::load_import_profile());
  EXPECT_EQ(profile.settings["id"], "default");
  auto telegram = take(app::load_import_profile("telegram"));
  EXPECT_EQ(telegram.settings["id"], "telegram");
  const auto project = fs::path(AM_GOLDEN_FIXTURES).parent_path().parent_path().parent_path();
  EXPECT_EQ(profile.sha256, hash(bytes(project / "profiles" / "default.toml")));
  EXPECT_EQ(telegram.sha256, hash(bytes(project / "profiles" / "telegram.toml")));
  const std::string source = "id=\"custom\"\r\nversion=1\r\n[boundaries]\r\ncandidate=\"root\"\r\n";
  write(root / "profile.toml", source);
  auto custom = take(app::load_import_profile((root / "profile.toml").string()));
  EXPECT_EQ(custom.sha256, hash(source));
  for (const auto& [text, pointer] : std::array<std::pair<std::string, std::string>, 5>{
           {{"id=\"bad\"\nversion=1\nunknown=true", "/unknown"},
            {"id=\"bad\"\nversion=1\n[boundaries]\ncontext_levels=\"one\"", "/boundaries/context_levels"},
            {"id=\"bad\"\nversion=1\n[provenance]\nurl_ad_patterns=[\"[\"]", "/provenance/url_ad_patterns"},
            {"id=\"bad\"\nversion=1\n[previews]\nprimary=7", "/previews/primary"},
            {"id=\"bad\"\nversion=1\n[limits]\nmax_ratio=nan", "/limits/max_ratio"}}}) {
    write(root / "bad.toml", text);
    auto invalid = app::load_import_profile((root / "bad.toml").string());
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().ctx.detail, pointer);
  }
}
TEST(ImportScan, ReadOnlyMatchingUsesNamesHashesAndNeverTouchesLibraryAssets) {
  RecordProperty("req", "SC-15|K-1");
  const auto work = fresh(), source = work / "source", library = work / "library";
  const std::string payload = "Unique matching payload";
  write(source / "Part.bin", payload);
  take(am::library::Library::init(library));
  {
    auto catalog = take(am::catalog::Catalog::open(library, capabilities()));
    am::repository::CreateRequest create;
    am::repository::AddFiles add;
    const auto input = am::Uuid::v4().to_string(), object = am::Uuid::v4().to_string(),
               file = am::Uuid::v4().to_string();
    add.input = {{"id", input},
                 {"imported_at", "2026-11-23T00:00:00.000Z"},
                 {"mode", "strict"},
                 {"completeness", "complete"},
                 {"objects", am::repository::Json::array({{{"id", object},
                                                           {"kind", "file"},
                                                           {"name", "part.bin"},
                                                           {"size_bytes", std::to_string(payload.size())},
                                                           {"sha256", hash(payload)}}})},
                 {"entries", am::repository::Json::array()}};
    am::repository::FileInput f;
    f.path = "Part.bin";
    f.source = source / "Part.bin";
    f.file_id = file;
    add.files.push_back(f);
    create.files.push_back(add);
    take(catalog.create(create));
  }
  const auto before_source = snapshot(source), before_library = library_snapshot(library);
  auto r = request({source / "Part.bin"});
  r.library = library;
  unsigned library_reads = 0;
  r.hooks.on_library_asset_read = [&](const auto&) { ++library_reads; };
  auto weak = run(r);
  ASSERT_EQ(weak["candidates"][0]["action"], "ask");
  EXPECT_EQ(weak["candidates"][0]["matches"][0]["kind"], "name_size");
  r.hash_inputs = r.skip_identical = true;
  auto exact = run(r);
  bool input_hash = false, file_hash = false;
  for (const auto& m : exact["candidates"][0]["matches"]) {
    input_hash = input_hash || m["kind"] == "sha256_input";
    file_hash = file_hash || m["kind"] == "sha256_file";
  }
  EXPECT_TRUE(input_hash);
  EXPECT_TRUE(file_hash);
  EXPECT_EQ(exact["candidates"][0]["action"], "skip");
  EXPECT_EQ(snapshot(source), before_source);
  EXPECT_EQ(library_snapshot(library), before_library);
  EXPECT_EQ(library_reads, 0u);
  write(source / "Other.bin", "not a known input");
  r.roots = {source};
  auto partial = run(r);
  EXPECT_EQ(partial["candidates"][0]["action"], "ask");
  fs::remove(source / "Other.bin");
  write(source / "hidden" / "unreadable", "hidden payload");
  r.hooks.before_io = [](const auto& path, imp::ScanIo io) -> am::Result<void> {
    if (path.filename() == "hidden" && io == imp::ScanIo::enumerate)
      return am::Error::make(am::Errc::io_error, "Injected enumeration failure").id("am.import.node_unavailable");
    return {};
  };
  auto incomplete = run(r);
  EXPECT_EQ(incomplete["candidates"][0]["action"], "ask");
  r.hash_inputs = false;
  EXPECT_FALSE(imp::scan(r));
}
TEST(ImportScan, LibraryOverlapIsRejectedInBothDirectionsBeforeTraversal) {
  RecordProperty("req", "SC-3|SC-15|K-1");
  const auto root = fresh(), library = root / "library";
  take(am::library::Library::init(library));
  for (const auto& source : std::vector<fs::path>{root, library / "assets"}) {
    auto r = request({source});
    r.library = library;
    unsigned operations = 0;
    r.hooks.before_io = [&](const auto&, imp::ScanIo) -> am::Result<void> {
      ++operations;
      return {};
    };
    auto result = imp::scan(r);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, am::Errc::invalid_argument);
    EXPECT_EQ(result.error().message_id, "am.import.library_overlaps_source");
    EXPECT_EQ(operations, 0u);
    auto publication = app::scan_import(r, fresh() / "plan.json");
    ASSERT_FALSE(publication);
    EXPECT_EQ(publication.error().message_id, "am.import.library_overlaps_source");
    EXPECT_EQ(operations, 0u);
  }
  const auto before = snapshot(root);
  auto without_matching = run(request({root}));
  EXPECT_FALSE(without_matching["nodes"].empty());
  EXPECT_EQ(snapshot(root), before);
}
TEST(ImportScan, SameAssetIdDistinguishesSameAndNewerRevision) {
  RecordProperty("req", "SC-14|SC-15");
  const auto work = fresh(), source = work / "source", first = work / "first", library = work / "library";
  fs::create_directories(source);
  take(am::library::Library::init(first));
  take(am::library::Library::init(library));
  const auto id = am::Uuid::v4().to_string();
  {
    auto a = take(am::catalog::Catalog::open(first, capabilities())),
         b = take(am::catalog::Catalog::open(library, capabilities()));
    am::repository::CreateRequest create;
    create.asset_id = id;
    auto original = take(a.create(create));
    take(b.create(create));
    am::repository::ChangeRequest change;
    change.ref = original.change.ref;
    change.expected_rev = 1;
    am::repository::SetMetadata edit;
    edit.field = "title";
    edit.value = am::repository::Json("Updated");
    change.ops.push_back(edit);
    take(a.change(change));
    fs::copy_file(first / "assets" / id.substr(34) / (id + ".zip"), source / "newer.zip");
    fs::copy_file(library / "assets" / id.substr(34) / (id + ".zip"), source / "same.zip");
  }
  const auto before = library_snapshot(library);
  auto r = request({source});
  r.library = library;
  auto p = run(r);
  std::set<std::string> relations;
  for (const auto& c : p["candidates"]) {
    EXPECT_EQ(c["kind"], "ready_asset");
    EXPECT_TRUE(reason(c, "ready_asset_manifest"));
    EXPECT_EQ(c["action"], "ask");
    for (const auto& m : c["matches"])
      if (m["kind"] == "same_asset_id") relations.insert(m["relation"].get<std::string>());
  }
  EXPECT_EQ(relations, (std::set<std::string>{"same_revision", "newer"}));
  EXPECT_EQ(library_snapshot(library), before);
}
TEST(ImportScan, MatchingReadsCommittedWalWhileWriterIsOpen) {
  RecordProperty("req", "SC-15|K-1");
  const auto work = fresh(), library = work / "library", source = work / "source";
  fs::create_directories(source);
  take(am::library::Library::init(library));
  auto writer = take(am::catalog::Catalog::open(library, capabilities()));
  am::repository::CreateRequest create;
  auto asset = take(writer.create(create));
  const auto id = asset.change.ref.asset_id;
  fs::copy_file(library / "assets" / id.substr(34) / (id + ".zip"), source / "ready.zip");
  const auto before = library_snapshot(library);
  auto r = request({source});
  r.library = library;
  auto p = run(r);
  ASSERT_EQ(p["candidates"].size(), 1u);
  bool matched = false;
  for (const auto& m : p["candidates"][0]["matches"]) matched = matched || m["kind"] == "same_asset_id";
  EXPECT_TRUE(matched);
  EXPECT_EQ(library_snapshot(library), before);
}
TEST(ImportScan, UnavailableDirectoryAsksAndLifecycleIsLoggedAfterCancellation) {
  RecordProperty("req", "SC-13|NFR-14.4.9");
  const auto root = fresh();
  write(root / "blocked" / "part", "payload");
  write(root / "good", "good");
  auto sink = std::make_shared<log_tests::MemorySink>();
  am::log::set_sink(sink);
  struct Reset {
    ~Reset() { am::log::set_sink(nullptr); }
  } reset;
  auto r = request({root});
  r.hooks.before_io = [](const auto& path, imp::ScanIo io) -> am::Result<void> {
    if (path.filename() == "blocked" && io == imp::ScanIo::enumerate)
      return am::Error::make(am::Errc::io_error, "Injected directory failure").id("am.import.node_unavailable");
    return {};
  };
  auto p = run(r);
  EXPECT_EQ(node(p, "blocked")["status"], "unavailable");
  EXPECT_TRUE(reason(p["candidates"][0], "has_unavailable"));
  EXPECT_EQ(p["candidates"][0]["action"], "ask");
  r = request({root});
  r.cancelled = [] { return true; };
  auto cancelled = imp::scan(r);
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().code, am::Errc::cancelled);
  bool stopped = false, path_kind = false;
  unsigned started = 0, finished = 0;
  for (const auto& row : sink->rows) {
    const auto record = J::parse(row);
    if (record["event"] == "import.scan.started") ++started;
    if (record["event"] == "import.scan.finished") {
      ++finished;
      stopped = stopped || record["fields"]["outcome"] == "stopped";
    }
    if (record["event"] == "import.scan.node_unavailable") path_kind = record["kinds"]["path"] == "path";
  }
  EXPECT_EQ(started, 2u);
  EXPECT_EQ(finished, 2u);
  EXPECT_TRUE(stopped);
  EXPECT_TRUE(path_kind);
}
TEST(ImportScan, MeasuresTenThousandFilesWithAndWithoutHashWithoutThreshold) {
  RecordProperty("req", "SC-3");
  const auto root = fresh();
  for (unsigned d = 0; d < 1000; ++d)
    for (unsigned f = 0; f < 10; ++f)
      write(root / std::to_string(d) / ("file" + std::to_string(f)), "synthetic payload");
  for (bool hash_inputs : {false, true}) {
    auto r = request({root});
    r.hash_inputs = hash_inputs;
    const auto start = std::chrono::steady_clock::now();
    auto p = take(imp::scan(r));
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(p.value["nodes"].size(), 11001u);
    std::cout << "IMPORT_SCAN_COST hash=" << hash_inputs << " nodes=" << p.value["nodes"].size()
              << " seconds=" << seconds << " us_per_node=" << seconds * 1e6 / 11001 << '\n';
  }
}
