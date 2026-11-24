#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>

#include "am/application/job_service.hpp"
#include "am/application/logging.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/unicode.hpp"
#include "am/foundation/uuid.hpp"
#include "am/storage/asset_zip/reader.hpp"
#include "log_test_sink.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace {
namespace fs = std::filesystem;
namespace app = am::application;
namespace l = am::log;
using app::Json;
template <class T>
T take(am::Result<T> value) {
  if (!value) throw std::runtime_error(am::to_log_string(value.error()));
  return std::move(*value);
}
std::string bytes(const fs::path& path) {
  std::ifstream input(am::paths::native(path), std::ios::binary);
  return {std::istreambuf_iterator<char>(input), {}};
}
void write(const fs::path& path, std::string_view data) {
  fs::create_directories(path.parent_path());
  std::ofstream out(am::paths::native(path), std::ios::binary);
  out << data;
  if (!out) throw std::runtime_error("Fixture write failed");
}
fs::path fresh() {
  const auto path = fs::path(AM_LOG_TEST_ROOT) / am::Uuid::v4().to_string();
  fs::create_directories(path);
  return path;
}
std::vector<fs::path> files(const fs::path& root) {
  std::vector<fs::path> result;
  for (const auto& file : fs::directory_iterator(root))
    if (file.path().extension() == ".jsonl") result.push_back(file.path());
  std::sort(result.begin(), result.end());
  return result;
}
std::vector<Json> rows(const fs::path& root) {
  std::vector<Json> result;
  for (const auto& path : files(root)) {
    std::ifstream input(am::paths::native(path), std::ios::binary);
    std::string line;
    while (std::getline(input, line)) result.push_back(Json::parse(line));
  }
  return result;
}
unsigned long pid() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<unsigned long>(getpid());
#endif
}
#ifdef _WIN32
struct OfflineFile {
  fs::path path;
  DWORD original;
  explicit OfflineFile(const fs::path& value)
      : path(am::paths::native(value)), original(GetFileAttributesW(path.c_str())) {
    if (original == INVALID_FILE_ATTRIBUTES || !SetFileAttributesW(path.c_str(), original | FILE_ATTRIBUTE_OFFLINE))
      throw std::runtime_error("Cannot set the offline attribute on the fixture");
  }
  ~OfflineFile() { SetFileAttributesW(path.c_str(), original); }
  OfflineFile(const OfflineFile&) = delete;
};
#endif
am::library::OpenOptions capabilities() {
  am::library::OpenOptions options;
  options.capabilities.emplace();
  options.capabilities->mode = am::repository::StorageMode::full;
  return options;
}
std::map<std::string, std::string> unzip(const fs::path& path) {
  auto archive = take(am::asset_zip::ZipArchive::open(take(am::asset_zip::file_source(path))));
  am::asset_zip::ReadBudget budget(UINT64_MAX);
  std::map<std::string, std::string> result;
  for (const auto& entry : archive.entries()) {
    auto stream = take(archive.open_entry(entry.index, budget));
    std::array<std::byte, 4096> block{};
    std::string content;
    for (;;) {
      auto n = take(stream.read(block));
      if (!n) break;
      content.append(reinterpret_cast<const char*>(block.data()), n);
    }
    result[entry.utf8_name] = std::move(content);
  }
  return result;
}
class FileLog : public testing::Test {
  void TearDown() override { l::set_sink(nullptr); }
};
class LogContext : public FileLog {};
class DiagExport : public FileLog {
 protected:
  void SetUp() override {
#ifdef _WIN32
    _putenv_s("AM_LOG_DIR", "");
#else
    unsetenv("AM_LOG_DIR");
#endif
  }
};
}  // namespace
TEST_F(FileLog, ProcessNameUtf8DirectoryRotationAndRetentionCount) {
  RecordProperty("req", "NFR-14.4.9");
  const auto root = fresh() / fs::path(u8"日志");
  app::LoggingOptions options;
  options.directory = root;
  options.max_file_bytes = 700;
  options.max_files_per_process = 3;
  auto sink = take(app::open_file_log(options));
  for (unsigned i = 0; i < 40; ++i)
    AM_LOG(info, "test", "rotation.test", "Rotation", {{"value", static_cast<std::uint64_t>(i)}});
  l::set_sink(nullptr);
  sink.reset();
  const auto logs = files(root);
  EXPECT_LE(logs.size(), 3u);
  EXPECT_GT(logs.size(), 1u);
  const std::regex pattern(R"(^am-[0-9]{8}T[0-9]{6}Z-[0-9]+(\.[0-9]+)?\.jsonl$)");
  for (const auto& path : logs) {
    EXPECT_TRUE(std::regex_match(path.filename().string(), pattern));
    EXPECT_NE(path.filename().string().find("-" + std::to_string(pid())), std::string::npos);
  }
  EXPECT_FALSE(rows(root).empty());
}
TEST_F(FileLog, TotalQuotaKeepsForeignNamesAndCurrentProcessAndDeletesOldest) {
  RecordProperty("req", "NFR-14.4.9");
  const auto root = fresh();
  const auto oldest = root / "am-20000101T000000Z-999999991.jsonl",
             newer = root / "am-20010101T000000Z-999999992.jsonl",
             own = root / ("am-19990101T000000Z-" + std::to_string(pid()) + ".2.jsonl");
  for (const auto& path : {oldest, newer, own}) write(path, std::string(64, 'x'));
  write(root / "notes.txt", "owner notes");
  write(root / "am-foo.jsonl", "foreign");
  fs::last_write_time(oldest, fs::file_time_type::clock::now() - std::chrono::hours(3));
  fs::last_write_time(newer, fs::file_time_type::clock::now() - std::chrono::hours(2));
  fs::last_write_time(own, fs::file_time_type::clock::now() - std::chrono::hours(4));
  app::LoggingOptions options;
  options.directory = root;
  options.max_total_bytes = 128;
  auto sink = take(app::open_file_log(options));
  l::set_sink(nullptr);
  sink.reset();
  EXPECT_FALSE(fs::exists(oldest));
  EXPECT_TRUE(fs::exists(newer));
  EXPECT_TRUE(fs::exists(own));
  EXPECT_EQ(bytes(root / "notes.txt"), "owner notes");
  EXPECT_EQ(bytes(root / "am-foo.jsonl"), "foreign");
}
TEST_F(FileLog, QuotaContinuesAfterAnInjectedRemovalFailure) {
  RecordProperty("req", "NFR-14.4.9");
  const auto root = fresh();
  const auto held = root / "am-20000101T000000Z-999999991.jsonl";
  const auto next = root / "am-20010101T000000Z-999999992.jsonl";
  write(held, std::string(64, 'h'));
  write(next, std::string(64, 'n'));
  fs::last_write_time(held, fs::file_time_type::clock::now() - std::chrono::hours(3));
  fs::last_write_time(next, fs::file_time_type::clock::now() - std::chrono::hours(2));
  std::vector<fs::path> attempted;
  app::LoggingOptions options;
  options.directory = root;
  options.max_total_bytes = 64;
  options.before_remove = [&](const auto& file) -> am::Result<void> {
    attempted.push_back(file.filename());
    if (file.filename() == held.filename()) return am::Error::make(am::Errc::io_error, "Injected deletion failure");
    return {};
  };
  auto sink = take(app::open_file_log(options));
  EXPECT_EQ(attempted, (std::vector<fs::path>{held.filename(), next.filename()}));
  EXPECT_TRUE(fs::exists(held));
  EXPECT_FALSE(fs::exists(next));
  EXPECT_EQ(bytes(held), std::string(64, 'h'));
}
TEST_F(FileLog, QuotaToleratesARealWindowsFileHeldWithoutShareDelete) {
  RecordProperty("req", "NFR-14.4.9");
#ifdef _WIN32
  const auto root = fresh();
  const auto held = root / "am-20000101T000000Z-999999991.jsonl";
  const auto next = root / "am-20010101T000000Z-999999992.jsonl";
  write(held, std::string(64, 'h'));
  write(next, std::string(64, 'n'));
  fs::last_write_time(held, fs::file_time_type::clock::now() - std::chrono::hours(3));
  fs::last_write_time(next, fs::file_time_type::clock::now() - std::chrono::hours(2));
  const auto handle = CreateFileW(am::paths::native(held).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  const std::shared_ptr<void> close(handle, [](void* value) { CloseHandle(value); });
  app::LoggingOptions options;
  options.directory = root;
  options.max_total_bytes = 64;
  auto sink = take(app::open_file_log(options));
  EXPECT_TRUE(fs::exists(held));
  EXPECT_FALSE(fs::exists(next));
  EXPECT_EQ(bytes(held), std::string(64, 'h'));
#else
  GTEST_SKIP() << "FILE_SHARE_DELETE denial is a Win32 sharing contract";
#endif
}
TEST_F(FileLog, WriteFailureCountsDroppedAndRecoveryReportsItWithoutThrowing) {
  RecordProperty("req", "NFR-14.4.9");
  const auto root = fresh();
  std::atomic<bool> fail = true;
  app::LoggingOptions options;
  options.directory = root;
  options.before_write = [&]() -> am::Result<void> {
    if (fail) return am::Error::make(am::Errc::io_error, "Injected write failure");
    return {};
  };
  auto sink = take(app::open_file_log(options));
  for (unsigned i = 0; i < 3; ++i) AM_LOG(error, "test", "failure.test", "Lost record");
  fail = false;
  AM_LOG(error, "test", "recovered.test", "Recovered record");
  auto current = rows(root);
  ASSERT_EQ(current.size(), 2u);
  EXPECT_EQ(current[0]["event"], "log.dropped");
  EXPECT_EQ(current[0]["fields"]["count"], 3);
  EXPECT_EQ(current[1]["event"], "recovered.test");
  l::set_sink(nullptr);
  sink.reset();
}
TEST_F(FileLog, UnavailableDirectoryReturnsWarningAndErrorFlushIsImmediate) {
  RecordProperty("req", "NFR-14.4.9");
  const auto root = fresh();
  write(root / "blocked", "file");
  app::LoggingOptions options;
  options.directory = root / "blocked";
  auto unavailable = app::open_file_log(options);
  ASSERT_FALSE(unavailable);
  EXPECT_EQ(unavailable.error().message_id, "am.log.open_failed");
  options.directory = root / "logs";
  auto sink = take(app::open_file_log(options));
  AM_LOG(error, "test", "flush.test", "Visible before closing");
  const auto visible = rows(options.directory);
  ASSERT_EQ(visible.size(), 1u);
  EXPECT_EQ(visible.front()["event"], "flush.test");
  l::set_sink(nullptr);
  sink.reset();
}
TEST_F(FileLog, ActualRotationFailureIsCountedAndLoggingRecovers) {
  RecordProperty("req", "NFR-14.4.9");
  const auto root = fresh();
  app::LoggingOptions options;
  options.directory = root;
  options.max_file_bytes = 512;
  options.max_files_per_process = 2;
  auto sink = take(app::open_file_log(options));
  AM_LOG(error, "test", "before.failure", "Before failure", {{"padding", std::string(300, 'x')}});
  auto present = files(root);
  ASSERT_EQ(present.size(), 1u);
  const auto obstacle = root / (present[0].stem().string() + ".1.jsonl");
  write(obstacle / "owned-marker.txt", "rotation obstacle");
  EXPECT_NO_THROW(AM_LOG(error, "test", "disk.failure", "Rotation fails"));
  EXPECT_TRUE(fs::exists(obstacle / "owned-marker.txt"));
  fs::remove(obstacle / "owned-marker.txt");
  fs::remove(obstacle);
  AM_LOG(error, "test", "disk.recovered", "Rotation recovered");
  const auto current = rows(root);
  bool notice = false, recovered = false;
  for (const auto& record : current) {
    if (record["event"] == "log.dropped") {
      notice = true;
      EXPECT_EQ(record["fields"]["count"], 1);
    }
    if (record["event"] == "disk.recovered") recovered = true;
  }
  EXPECT_TRUE(notice);
  EXPECT_TRUE(recovered);
  l::set_sink(nullptr);
  sink.reset();
}
TEST_F(FileLog, MeasuresInfoFileCostWithoutThreshold) {
  RecordProperty("req", "NFR-14.4.9");
  app::LoggingOptions options;
  options.directory = fresh();
  auto sink = take(app::open_file_log(options));
  constexpr unsigned count = 2000;
  const auto start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < count; ++i)
    AM_LOG(info, "test", "cost.test", "Info record", {{"sequence", static_cast<std::uint64_t>(i)}});
  const auto end = std::chrono::steady_clock::now();
  std::cout << "LOG_COST file_info_ns=" << std::chrono::duration<double, std::nano>(end - start).count() / count
            << '\n';
  l::set_sink(nullptr);
  sink.reset();
  EXPECT_EQ(rows(options.directory).size(), count);
}
TEST_F(LogContext, JobAndWorkerProgressHaveJobAndLibraryIdentity) {
  RecordProperty("req", "NFR-14.4.9");
  const auto root = fresh() / "library";
  take(am::library::Library::init(root));
  auto memory = std::make_shared<log_tests::MemorySink>();
  l::set_sink(memory);
  auto service = take(app::JobService::open(root, {}, capabilities()));
  am::contracts::JobSpec spec;
  spec.type = "dev.probe";
  spec.params_json = "{\"steps\":2}";
  const auto job = take(service.enqueue(spec));
  take(service.run());
  const auto library = service.catalog().library().config().library_id;
  std::set<std::string> found;
  bool worker = false;
  for (std::size_t n = 0; n < memory->rows.size(); ++n) {
    const auto record = Json::parse(memory->rows[n]);
    const auto event = record["event"].get<std::string>();
    if (event.starts_with("job.")) {
      found.insert(event);
      EXPECT_EQ(record["ctx"]["job_id"], job.job_id);
      EXPECT_EQ(record["ctx"]["library_id"], library);
      if (memory->threads[n] != std::this_thread::get_id()) worker = true;
    }
  }
  EXPECT_TRUE(found.contains("job.started"));
  EXPECT_TRUE(found.contains("job.finished"));
  EXPECT_TRUE(found.contains("job.progress"));
  EXPECT_TRUE(worker);
}
TEST_F(LogContext, CatalogContextAndCommitPhasesRespectDebugFiltering) {
  RecordProperty("req", "NFR-14.4.9");
  const auto root = fresh() / "library";
  take(am::library::Library::init(root));
  auto memory = std::make_shared<log_tests::MemorySink>();
  memory->minimum = l::Level::info;
  l::set_sink(memory);
  auto catalog = take(am::catalog::Catalog::open(root, capabilities()));
  am::repository::CreateRequest request;
  request.metadata = Json{{"title", "Context fixture"}};
  take(catalog.create(request));
  for (const auto& text : memory->rows) EXPECT_NE(Json::parse(text)["event"], "commit.phase");
  memory->minimum = l::Level::debug;
  l::set_sink(memory);
  auto asset = take(catalog.create(request));
  std::set<std::string> phases;
  for (const auto& text : memory->rows) {
    const auto record = Json::parse(text);
    if (record["event"] == "commit.phase") {
      phases.insert(record["fields"]["phase"]);
      EXPECT_EQ(record["ctx"]["asset_id"], asset.change.ref.asset_id);
      EXPECT_EQ(record["ctx"]["library_id"], catalog.library().config().library_id);
    }
  }
  EXPECT_EQ(phases, (std::set<std::string>{"prepared", "verified", "committed", "indexed"}));
}
TEST_F(DiagExport, RedactsAllArchiveEntriesCorrelatesTokensAndPreservesExtensionsAndUuids) {
  RecordProperty("req", "NFR-14.4.9|K-130");
  const auto root = fresh();
  const auto logs = root / "logs";
  const std::string windows = "C:\\Users\\PrivateOwnerCanary\\private.stl",
                    posix = "/home/PrivateOwnerCanary/private.stl", unc = "\\\\PrivateHostCanary\\share\\private.stl",
                    identifier = "e3157b5d-baa6-4a69-a082-124034ff411a";
  Json source{{"ts", "2026-11-23T10:00:00.000Z"},
              {"msg", "PRIVATEOWNERCANARY on privatehostcanary"},
              {"ctx", {{"asset_id", identifier}}},
              {"fields",
               {{"first", windows},
                {"second", windows},
                {"posix", posix},
                {"unc", unc},
                {"mesh", "PrivateOwnerCanary model.stl"}}},
              {"kinds", {{"first", "path"}, {"second", "path"}, {"mesh", "name"}}},
              {"error", {{"detail", "sensitive relative location"}}}};
  write(logs / "am-20261123T000000Z-999991.jsonl", source.dump() + "\nnot JSON\n{\"fields\":{\"huge\":1e9999}}\n");
  app::DiagnosticOptions options;
  options.output = root / "first.zip";
  options.log_directory = logs;
  options.users = {"PrivateOwnerCanary"};
  options.hosts = {"PrivateHostCanary"};
  std::vector<fs::path> reads;
  options.on_read = [&](const auto& path) { reads.push_back(path); };
  const auto report = take(app::export_diagnostics(options));
  const auto archive = unzip(options.output);
  ASSERT_EQ(archive.size(), 2u);
  std::string joined;
  for (const auto& [name, content] : archive) {
    joined += name + content;
  }
  for (const auto& secret : {windows, posix, unc, std::string("sensitive relative location")})
    EXPECT_EQ(joined.find(secret), std::string::npos);
  EXPECT_EQ(am::unicode::path_collision_key(joined).find("privateownercanary"), std::string::npos);
  EXPECT_EQ(am::unicode::path_collision_key(joined).find("privatehostcanary"), std::string::npos);
  EXPECT_NE(joined.find(identifier), std::string::npos);
  EXPECT_NE(joined.find("1e9999"), std::string::npos);
  const auto& content = archive.at("logs/am-20261123T000000Z-999991.jsonl");
  const auto first = Json::parse(content.substr(0, content.find('\n')));
  EXPECT_EQ(first["fields"]["first"], first["fields"]["second"]);
  EXPECT_TRUE(first["fields"]["mesh"].get<std::string>().ends_with(".stl"));
  EXPECT_TRUE(first["msg"].get<std::string>().find("<user>") != std::string::npos);
  const auto metadata = Json::parse(archive.at("diagnostics.json"));
  EXPECT_EQ(metadata["logs"][0]["records"], 2);
  EXPECT_EQ(metadata["logs"][0]["dropped_lines"], 1);
  EXPECT_EQ(metadata["redaction"].size(), 2u);
  options.output = root / "second.zip";
  take(app::export_diagnostics(options));
  const auto second_bytes = unzip(options.output).at("logs/am-20261123T000000Z-999991.jsonl");
  const auto second = Json::parse(second_bytes.substr(0, second_bytes.find('\n')));
  EXPECT_NE(first["fields"]["first"], second["fields"]["first"]);
  EXPECT_EQ(reads.size(), 2u);
  EXPECT_GT(report.path_replacements, 0u);
  write(fs::path(AM_DIAG_CASE_ROOT) / "diagnostics.json", archive.at("diagnostics.json"));
}
TEST_F(DiagExport, OfflineLogIsNotOpenedAndIsReportedInsideTheSuccessfulExport) {
  RecordProperty("req", "NFR-14.4.9|K-130");
#ifdef _WIN32
  const auto root = fresh(), logs = root / "logs";
  const auto name = "am-20261124T000000Z-999999991.jsonl";
  const auto cloud = logs / name;
  write(cloud, "PrivateCloudContentCanary");
  OfflineFile offline(cloud);
  ASSERT_TRUE(take(am::paths::node_info(cloud)).cloud_placeholder);
  app::DiagnosticOptions options;
  options.output = root / "cloud-log.zip";
  options.log_directory = logs;
  unsigned opened = 0;
  options.on_read = [&](const auto& path) {
    if (path.filename() == cloud.filename()) ++opened;
  };
  const auto report = take(app::export_diagnostics(options));
  EXPECT_EQ(opened, 0u);
  ASSERT_EQ(report.unavailable.size(), 1u);
  EXPECT_EQ(report.unavailable[0].reason, "cloud_placeholder");
  const auto archive = unzip(options.output);
  EXPECT_FALSE(archive.contains(std::string("logs/") + name));
  const auto metadata = Json::parse(archive.at("diagnostics.json"));
  EXPECT_TRUE(metadata["logs"].empty());
  EXPECT_EQ(metadata["unavailable"][0],
            (Json{{"source_kind", "log"}, {"name", name}, {"reason", "cloud_placeholder"}}));
  for (const auto& [entry, content] : archive) {
    (void)entry;
    EXPECT_EQ(content.find("PrivateCloudContentCanary"), std::string::npos);
  }
  write(fs::path(AM_DIAG_CASE_ROOT) / "offline-log.json", archive.at("diagnostics.json"));
#else
  GTEST_SKIP()
      << "An actual FILE_ATTRIBUTE_OFFLINE fixture requires Windows; raw metadata mapping is tested on every platform";
#endif
}
TEST_F(DiagExport, OfflineDatabaseIsNotOpenedAndLibrarySummaryIsOmitted) {
  RecordProperty("req", "NFR-14.4.9|K-130");
#ifdef _WIN32
  const auto root = fresh(), library = root / "library";
  take(am::library::Library::init(library));
  const auto database = library / ".am/index.sqlite";
  OfflineFile offline(database);
  ASSERT_TRUE(take(am::paths::node_info(database)).cloud_placeholder);
  app::DiagnosticOptions options;
  options.output = root / "cloud-database.zip";
  options.library = library;
  unsigned opened = 0;
  options.on_read = [&](const auto&) { ++opened; };
  const auto report = take(app::export_diagnostics(options));
  EXPECT_EQ(opened, 0u);
  ASSERT_EQ(report.unavailable.size(), 1u);
  const auto archive = unzip(options.output);
  EXPECT_FALSE(archive.contains("library.json"));
  const auto metadata = Json::parse(archive.at("diagnostics.json"));
  EXPECT_FALSE(metadata.contains("library"));
  EXPECT_EQ(metadata["unavailable"][0],
            (Json{{"source_kind", "database"}, {"name", "index.sqlite"}, {"reason", "cloud_placeholder"}}));
  write(fs::path(AM_DIAG_CASE_ROOT) / "offline-database.json", archive.at("diagnostics.json"));
#else
  GTEST_SKIP()
      << "An actual FILE_ATTRIBUTE_OFFLINE fixture requires Windows; raw metadata mapping is tested on every platform";
#endif
}
TEST_F(DiagExport, NonLinkReparseTagsRemainReadableThroughTheSharedRawMetadataHook) {
  RecordProperty("req", "NFR-14.4.9|K-130");
  const auto root = fresh(), logs = root / "logs";
  const auto file = logs / "am-20261124T000000Z-999999991.jsonl";
  write(file, l::serialize({l::Level::info, "test", "test.reparse", "Local reparse fixture", {}, nullptr, {}}));
  for (const std::uint32_t tag : {0x80000013u, 0x80000018u, 0x8000abcdu}) {
    app::DiagnosticOptions options;
    options.output = root / (std::to_string(tag) + ".zip");
    options.log_directory = logs;
    options.node_data = [&](const auto& path) -> am::Result<am::paths::WindowsNodeData> {
      const auto actual = am::paths::node_info(path);
      if (!actual) return actual.error();
      am::paths::WindowsNodeData raw;
      raw.size = actual->size;
      raw.mtime_ns = actual->mtime_ns;
      raw.attributes = actual->kind == am::paths::NodeKind::directory ? 0x10 : 0;
      if (path.filename() == file.filename()) {
        raw.attributes |= 0x400;
        raw.reparse_tag = tag;
      }
      return raw;
    };
    unsigned opened = 0;
    options.on_read = [&](const auto&) { ++opened; };
    const auto report = take(app::export_diagnostics(options));
    EXPECT_EQ(opened, 1u);
    EXPECT_TRUE(report.unavailable.empty());
    EXPECT_TRUE(report.skipped_summary.empty());
    const auto archive = unzip(options.output);
    EXPECT_TRUE(archive.contains("logs/" + file.filename().string()));
    const auto metadata = Json::parse(archive.at("diagnostics.json"));
    EXPECT_EQ(metadata["logs"][0]["records"], 1);
    EXPECT_FALSE(metadata.contains("unavailable"));
  }
}
TEST_F(DiagExport, SinceUsesUtcOffsetsAndFractionalPrecisionAndCountsBrokenTimes) {
  RecordProperty("req", "NFR-14.4.9|K-130");
  const auto root = fresh();
  const auto logs = root / "logs";
  write(logs / "am-20261123T000000Z-999991.jsonl",
        "{\"ts\":\"2026-11-23T01:00:00+01:00\"}\n{\"ts\":\"2026-11-23T00:00:00.5Z\"}\n{\"ts\":\"2026-11-23T00:00:00."
        "501Z\"}\n{\"ts\":\"bad\"}\n");
  app::DiagnosticOptions options;
  options.output = root / "filtered.zip";
  options.log_directory = logs;
  options.since = "2026-11-23T00:00:00.500Z";
  take(app::export_diagnostics(options));
  const auto archive = unzip(options.output);
  const auto metadata = Json::parse(archive.at("diagnostics.json"));
  EXPECT_EQ(metadata["logs"][0]["records"], 2);
  EXPECT_EQ(metadata["logs"][0]["dropped_lines"], 1);
  options.since = "2026-02-30T12:00:00Z";
  options.output = root / "invalid.zip";
  auto failed = app::export_diagnostics(options);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().code, am::Errc::invalid_argument);
  EXPECT_FALSE(fs::exists(options.output));
}
TEST_F(DiagExport, DryRunDoesNotWriteAndExistingOutputAndUnavailableSourceFail) {
  RecordProperty("req", "NFR-14.4.9|K-130");
  const auto root = fresh();
  app::DiagnosticOptions options;
  options.output = root / "not-created/out.zip";
  options.dry_run = true;
  const auto report = take(app::export_diagnostics(options));
  EXPECT_FALSE(report.logs_available);
  EXPECT_EQ(report.entries, (std::vector<std::string>{"diagnostics.json"}));
  EXPECT_FALSE(fs::exists(root / "not-created"));
  options.dry_run = false;
  options.output = root / "existing.zip";
  write(options.output, "owner output");
  auto existing = app::export_diagnostics(options);
  ASSERT_FALSE(existing);
  EXPECT_EQ(existing.error().message_id, "am.diag.output_exists");
  EXPECT_EQ(bytes(options.output), "owner output");
  options.dry_run = true;
  auto existing_preview = app::export_diagnostics(options);
  ASSERT_FALSE(existing_preview);
  EXPECT_EQ(existing_preview.error().message_id, "am.diag.output_exists");
  options.dry_run = false;
  options.output = root / "absent.zip";
  options.log_directory = root / "missing";
  EXPECT_FALSE(app::export_diagnostics(options));
  EXPECT_FALSE(fs::exists(options.output));
}
TEST_F(DiagExport, RemoteSourceIsRejectedBeforeAnyInputRead) {
  RecordProperty("req", "NFR-14.4.9|K-130");
  const auto root = fresh();
  app::DiagnosticOptions options;
  options.output = root / "out.zip";
  options.log_directory = fs::path(u8"\\\\private-host.invalid\\share\\logs");
  unsigned reads = 0;
  options.on_read = [&](const auto&) { ++reads; };
  auto result = app::export_diagnostics(options);
  ASSERT_FALSE(result);
  EXPECT_EQ(reads, 0u);
  EXPECT_FALSE(fs::exists(options.output));
}
TEST_F(DiagExport, NativeWindowsSourcesRetainTheirLogEntries) {
  RecordProperty("req", "NFR-14.4.9|K-130");
  const auto root = fresh();
  const auto logs = root / "logs";
  write(logs / "am-20261123T000000Z-444.jsonl", "{\"msg\":\"Native source\"}\n");
  app::DiagnosticOptions options;
  options.output = root / "native.zip";
  options.log_directory = am::paths::native(logs);
  auto result = app::export_diagnostics(options);
  ASSERT_TRUE(result);
  const auto archive = unzip(options.output);
  ASSERT_TRUE(archive.contains("logs/am-20261123T000000Z-444.jsonl"));
}
TEST_F(DiagExport, UnicodeWordBoundariesAndCaseFoldingKeepLongerWords) {
  RecordProperty("req", "NFR-14.4.9|K-130");
  const auto root = fresh();
  const auto logs = root / "logs";
  write(logs / "am-20261123T000000Z-333.jsonl",
        Json{{"msg", "cAnArYuSeR — CANARYHOST; xCanaryUser; КирИлЛ—кириллов; CanaryUser-PC Pat.Owner"}}.dump() + "\n");
  app::DiagnosticOptions options;
  options.output = root / "out.zip";
  options.log_directory = logs;
  options.users = {"CanaryUser", "Кирилл", "Pat.Owner"};
  options.hosts = {"CanaryHost", "CanaryUser-PC", "Owner"};
  take(app::export_diagnostics(options));
  const auto content = unzip(options.output).at("logs/am-20261123T000000Z-333.jsonl");
  EXPECT_EQ(Json::parse(content)["msg"], "<user> — <host>; xCanaryUser; <user>—кириллов; <host> <user>");
}
TEST_F(DiagExport, LibraryIsReadOnlyIncludesLastThousandLogEntriesAndNeverReadsAssetZips) {
  RecordProperty("req", "NFR-14.4.9|K-130");
  const auto root = fresh(), library = root / "library";
  take(am::library::Library::init(library));
  {
    auto catalog = take(am::catalog::Catalog::open(library, capabilities()));
    am::repository::CreateRequest request;
    request.metadata = Json{{"title", "PrivateOwnerCanary asset"}};
    take(catalog.create(request));
    for (unsigned i = 0; i < 1002; ++i) {
      am::db::LogEntry entry;
      entry.code = "test.entry";
      entry.level = "info";
      entry.message = "PrivateOwnerCanary message";
      entry.detail_json = "{\"path\":\"C:\\\\private\\\\file.stl\"}";
      take(catalog.operation_log().append(entry));
    }
  }
  std::map<fs::path, std::string> before;
  for (const auto& file : fs::recursive_directory_iterator(library))
    if (file.is_regular_file() && (file.path().extension() == ".zip" || file.path().extension() == ".sqlite" ||
                                   file.path().filename() == "am-library.toml"))
      before[file.path()] = bytes(file.path());
  app::DiagnosticOptions options;
  options.output = root / "library-diag.zip";
  options.library = library;
  options.users = {"PrivateOwnerCanary"};
  std::vector<fs::path> reads;
  options.on_read = [&](const auto& path) { reads.push_back(path); };
  take(app::export_diagnostics(options));
  const auto archive = unzip(options.output);
  ASSERT_TRUE(archive.contains("library.json"));
  const auto summary = Json::parse(archive.at("library.json"));
  ASSERT_EQ(summary["op_log"].size(), 1000u);
  EXPECT_EQ(summary["op_log"].back()["seq"].get<std::int64_t>() - summary["op_log"].front()["seq"].get<std::int64_t>(),
            999);
  EXPECT_FALSE(summary["assets"].empty());
  for (const auto& [path, data] : before) EXPECT_EQ(bytes(path), data);
  for (const auto& path : reads) EXPECT_NE(path.extension(), ".zip");
  EXPECT_EQ(archive.at("library.json").find("PrivateOwnerCanary"), std::string::npos);
}
