#include <gtest/gtest.h>
#include "am/storage/commit/protocol.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

using namespace am;
using namespace am::commit;
namespace {
using Path = std::filesystem::path;
template<class T> T value(Result<T> r) { if (!r) throw std::runtime_error(r.error().message_id + ": " + r.error().ctx.detail); return std::move(r).value(); }
void value(Result<void> r) { if (!r) throw r.error(); }
std::string id() { return Uuid::v4().to_string(); }
Path fresh() { auto p = Path(AM_COMMIT_TEST_ROOT) / ("commit-" + id()); std::filesystem::create_directories(p); return p; }
std::string manifest(std::string_view title) { return R"({"format":{"id":"urn:uuid:0d40b3a5-39ae-48e6-a8e8-da18b1b8c128","version":{"major":1,"minor":0}},"title":")" + std::string(title) + "\"}"; }
void add(asset_zip::ZipWriter& writer, std::string_view name, std::string_view data) {
  asset_zip::EntryOptions o; o.method = asset_zip::CompressionMethod::stored; o.uncompressed_size = data.size(); auto e = value(writer.begin_entry(name, o)); value(e.write(std::as_bytes(std::span(data)))); value(e.finish());
}
void produce(asset_zip::ZipWriter& writer, std::string_view title = "new", char byte = 'n') {
  add(writer, "manifest.json", manifest(title));
  asset_zip::EntryOptions o; o.method = asset_zip::CompressionMethod::stored; o.uncompressed_size = 4096;
  auto entry = value(writer.begin_entry("content/body", o)); const std::string half(2048, byte);
  value(entry.write(std::as_bytes(std::span(half)))); value(entry.write(std::as_bytes(std::span(half)))); value(entry.finish());
}
void write_version(const Path& path, std::string_view title = "old", char byte = 'o') {
  std::filesystem::remove(am::paths::native(path)); auto sink = value(asset_zip::file_sink(path)); auto writer = value(asset_zip::ZipWriter::create(sink)); produce(writer, title, byte); value(writer.finish());
}
std::vector<std::byte> bytes(const Path& path) {
  std::ifstream in(am::paths::native(path), std::ios::binary); if (!in) throw std::runtime_error("Read fixture failed");
  std::string text((std::istreambuf_iterator<char>(in)), {}); auto data = std::as_bytes(std::span(text)); return {data.begin(), data.end()};
}
void valid(const Path& path) {
  auto source = value(asset_zip::file_source(path, true)); auto archive = value(asset_zip::ZipArchive::open(source)); asset_zip::ReadBudget budget(1000000);
  for (const auto& e : archive.entries()) value(archive.verify_entry(e.index, budget, true));
}
CommitRequest request(const Path& root, const std::string& asset, asset_zip::ReadBudget& budget) {
  CommitRequest r(budget); r.library_root = root; r.target_path = root / "asset.zip"; r.asset_id = asset; r.op_id = id(); r.estimated_size = 6000; r.hooks.mid_write_after_bytes = 2250;
#ifndef _WIN32
  // Synthetic protocol tests do not certify the filesystem (fs_support.md v0.2).
  r.capabilities.mode = StorageMode::full;
#endif
  if (std::filesystem::exists(r.target_path)) r.base = value(inspect_zip(r.target_path, budget)); else r.kind = "create";
  r.producer = [](asset_zip::ZipWriter& w) -> Result<void> { produce(w); return {}; }; return r;
}
void clean(const Path& root) {
  auto journal = value(db::StateJournal::open(root)); EXPECT_TRUE(value(journal->list()).empty());
  const auto dir = root / ".am/.am-tmp"; if (std::filesystem::exists(dir)) EXPECT_TRUE(std::filesystem::is_empty(dir));
}
Error injected() { return Error::make(Errc::io_error, "Injected failure", "Test checkpoint").id("test.commit.injected"); }
constexpr std::array points{FaultPoint::after_lock, FaultPoint::after_prepared, FaultPoint::mid_write, FaultPoint::after_finish, FaultPoint::after_verify,
    FaultPoint::after_verified_journal, FaultPoint::before_replace, FaultPoint::after_replace, FaultPoint::after_committed_journal, FaultPoint::after_indexed_journal};
#ifdef _WIN32
class Child {
 public:
  HANDLE process = nullptr;
  Child(std::string_view mode, const Path& root, std::string_view asset, std::string_view op_or_ready, std::string_view point = {}) {
    const auto exe = Path(AM_COMMIT_HELPER).wstring();
    auto wide = [](std::string_view s) { return std::wstring(s.begin(), s.end()); };
    std::wstring command = L"\"" + exe + L"\" " + wide(mode) + L" \"" + root.wstring() + L"\" " + wide(asset) + L" \"" + wide(op_or_ready) + L"\"";
    if (!point.empty()) command += L" " + wide(point);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE; PROCESS_INFORMATION result{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &result)) throw std::runtime_error("CreateProcessW failed");
    process = result.hProcess; CloseHandle(result.hThread);
  }
  ~Child() { if (process) { if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) { TerminateProcess(process, 92); WaitForSingleObject(process, 30000); } CloseHandle(process); } }
  DWORD wait() { if (WaitForSingleObject(process, 30000) != WAIT_OBJECT_0) throw std::runtime_error("Crash helper timed out"); DWORD code = 0; GetExitCodeProcess(process, &code); return code; }
  void kill() { TerminateProcess(process, 92); (void)wait(); }
};
void ready(const Path& file) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!std::filesystem::exists(file)) { if (std::chrono::steady_clock::now() >= end) throw std::runtime_error("Lock holder did not become ready"); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
}
#endif
class FaultSink final : public asset_zip::RandomAccessSink {
 public:
  std::shared_ptr<asset_zip::RandomAccessSink> inner;
  bool corrupt = false;
  Result<void> write_at(std::uint64_t offset, std::span<const std::byte> data) override {
    if (data.size() >= 2048) {
      if (!corrupt) return Error::make(Errc::insufficient_space, "Injected full volume").id("am.zip.write.insufficient_space");
      std::vector<std::byte> modified(data.begin(), data.end()); modified[0] ^= std::byte{1}; return inner->write_at(offset, modified);
    }
    return inner->write_at(offset, data);
  }
  Result<void> flush() override { return inner->flush(); } Result<void> close() override { return inner->close(); }
  Result<void> discard() override { return inner->discard(); } std::uint64_t size() const noexcept override { return inner->size(); }
};
}
TEST(CommitProtocol, CreateUpdateVerifyEvidenceAndCallbacks) {
  RecordProperty("req", "ADR-0006|ADR-0025");
  const auto root = fresh(); const auto asset = id(); asset_zip::ReadBudget budget(1000000); auto r = request(root, asset, budget); int calls = 0;
  r.verifier = [&](const auto& archive) -> Result<void> { ++calls; EXPECT_EQ(archive.entries().size(), 2u); return {}; };
  r.on_committed = [&](const auto& row) -> Result<void> { ++calls; EXPECT_EQ(row.state, db::CommitState::committed); return {}; };
  auto created = commit_zip(r); ASSERT_TRUE(created) << created.error().ctx.detail; EXPECT_EQ(calls, 2); EXPECT_EQ(created->op_id, r.op_id); EXPECT_EQ(created->size, std::filesystem::file_size(root / "asset.zip"));
#ifdef _WIN32
  EXPECT_EQ(created->replacement, Replacement::posix_rename);
#else
  EXPECT_EQ(created->replacement, Replacement::rename);
#endif
  auto actual = value(inspect_zip(root / "asset.zip", budget)); EXPECT_EQ(actual.manifest_sha256, created->manifest_sha256); EXPECT_EQ(actual.mtime_ns, created->mtime_ns); valid(root / "asset.zip"); clean(root);
  auto update = request(root, asset, budget); update.producer = [](auto& w) -> Result<void> { produce(w, "two", 't'); return {}; };
  auto updated = commit_zip(update); ASSERT_TRUE(updated) << updated.error().ctx.detail; EXPECT_NE(updated->manifest_sha256, created->manifest_sha256); clean(root);
}
TEST(CommitProtocol, EveryPreReplaceInjectedErrorRollsBackWithoutChangingTarget) {
  RecordProperty("req", "ADR-0006");
  const auto root = fresh(); const auto asset = id(); write_version(root / "asset.zip"); const auto original = bytes(root / "asset.zip");
  for (const auto point : points) {
    if (point == FaultPoint::after_replace || point == FaultPoint::after_committed_journal || point == FaultPoint::after_indexed_journal) continue;
    SCOPED_TRACE(point_name(point)); asset_zip::ReadBudget budget(1000000); auto r = request(root, asset, budget);
    r.hooks.point = [=](FaultPoint p) -> Result<void> { if (p == point) return injected(); return {}; };
    auto result = commit_zip(r); ASSERT_FALSE(result); EXPECT_EQ(bytes(root / "asset.zip"), original); clean(root);
  }
}
TEST(CommitProtocol, ProducerWriterVerifierAndBuiltinFailuresPreserveTarget) {
  RecordProperty("req", "ADR-0006");
  const auto root = fresh(); const auto asset = id(); write_version(root / "asset.zip"); const auto original = bytes(root / "asset.zip");
  for (int failure = 0; failure < 7; ++failure) {
    asset_zip::ReadBudget budget(1000000); auto r = request(root, asset, budget);
    if (failure == 0) r.producer = [](auto&) -> Result<void> { return injected(); };
    if (failure == 1) r.producer = [](auto& w) -> Result<void> { asset_zip::EntryOptions o; o.uncompressed_size = 1; auto entry = value(w.begin_entry("manifest.json", o)); return entry.finish().error(); };
    if (failure == 2) r.verifier = [](const auto&) -> Result<void> { return injected(); };
    if (failure == 3) r.producer = [](auto& w) -> Result<void> { add(w, "manifest.json", "{}"); return {}; };
    if (failure == 4) r.hooks.create_sink = [](const Path& p) -> Result<std::shared_ptr<asset_zip::RandomAccessSink>> { auto s = std::make_shared<FaultSink>(); s->inner = value(asset_zip::file_sink(p)); s->corrupt = true; return std::shared_ptr<asset_zip::RandomAccessSink>(s); };
    if (failure == 5) r.verifier = [](const auto&) -> Result<void> { throw std::runtime_error("Injected verifier exception"); };
    if (failure == 6) r.verifier = [](const auto&) -> Result<void> { throw 42; };
    auto result = commit_zip(r); ASSERT_FALSE(result); EXPECT_EQ(bytes(root / "asset.zip"), original); clean(root);
    if (failure >= 2) { EXPECT_EQ(result.error().message_id, "am.commit.verification_failed"); EXPECT_TRUE(result.error().cause); }
  }
}
TEST(CommitProtocol, BaseChangesAndInvalidPathsAreRejected) {
  RecordProperty("req", "ADR-0006|ADR-0025");
  const auto root = fresh(); const auto asset = id(); write_version(root / "asset.zip"); asset_zip::ReadBudget budget(1000000);
  auto before = request(root, asset, budget); write_version(root / "asset.zip", "two", 't'); auto changed = bytes(root / "asset.zip");
  auto result = commit_zip(before); ASSERT_FALSE(result); EXPECT_EQ(result.error().message_id, "am.commit.base_changed"); EXPECT_EQ(bytes(root / "asset.zip"), changed);
  auto during = request(root, asset, budget); during.hooks.point = [&](FaultPoint p) -> Result<void> { if (p == FaultPoint::after_verify) write_version(root / "asset.zip", "out", 'x'); return {}; };
  result = commit_zip(during); ASSERT_FALSE(result); EXPECT_EQ(result.error().message_id, "am.commit.base_changed"); valid(root / "asset.zip"); EXPECT_NE(bytes(root / "asset.zip"), changed); clean(root);
  auto outside = request(root, asset, budget); outside.target_path = root.parent_path() / "outside.zip"; result = commit_zip(outside); ASSERT_FALSE(result); EXPECT_EQ(result.error().message_id, "am.commit.invalid_path");
}
TEST(CommitProtocol, PostReplaceCallbackErrorsRemainRecoverable) {
  RecordProperty("req", "ADR-0006|ADR-0025");
  for (int phase = 0; phase < 3; ++phase) {
    const auto root = fresh(); const auto asset = id(); write_version(root / "asset.zip"); const auto old = bytes(root / "asset.zip");
    asset_zip::ReadBudget budget(1000000); auto r = request(root, asset, budget);
    if (phase == 0) r.hooks.point = [](FaultPoint p) -> Result<void> { if (p == FaultPoint::after_replace) throw 42; return {}; };
    else r.on_committed = [](const auto&) -> Result<void> { return injected(); };
    auto failed = commit_zip(r); ASSERT_FALSE(failed); valid(root / "asset.zip"); EXPECT_NE(bytes(root / "asset.zip"), old);
    auto journal = value(db::StateJournal::open(root)); ASSERT_EQ(value(journal->list()).size(), 1u);
    RecoveryOptions options; int calls = 0;
    options.on_committed = [&](const auto&) -> Result<void> { ++calls; if (phase == 2) throw 42; return {}; };
    auto report = value(recover(root, options)); ASSERT_EQ(report.size(), 1u); EXPECT_EQ(calls, 1);
    EXPECT_EQ(report[0].outcome, phase == 0 ? RecoveryOutcome::already_committed : phase == 1 ? RecoveryOutcome::completed : RecoveryOutcome::failed);
    if (phase == 2) { ASSERT_EQ(value(journal->list()).size(), 1u); options.on_committed = {}; report = value(recover(root, options)); EXPECT_EQ(report[0].outcome, RecoveryOutcome::completed); }
    clean(root);
  }
}
TEST(CommitProtocol, SpaceBeforeAndDuringWriteLeavesNoOperation) {
  RecordProperty("req", "K-117|ADR-0033");
  for (int phase = 0; phase < 2; ++phase) {
    const auto root = fresh(); const auto asset = id(); write_version(root / "asset.zip"); const auto old = bytes(root / "asset.zip"); asset_zip::ReadBudget budget(1000000); auto r = request(root, asset, budget);
    if (phase == 0) r.hooks.free_space = [](const Path&) -> Result<std::uint64_t> { return std::uint64_t{0}; };
    else r.hooks.create_sink = [](const Path& p) -> Result<std::shared_ptr<asset_zip::RandomAccessSink>> { auto s = std::make_shared<FaultSink>(); s->inner = value(asset_zip::file_sink(p)); return std::shared_ptr<asset_zip::RandomAccessSink>(s); };
    auto result = commit_zip(r); ASSERT_FALSE(result); EXPECT_EQ(result.error().code, Errc::insufficient_space); EXPECT_EQ(bytes(root / "asset.zip"), old);
    if (phase == 0) EXPECT_FALSE(std::filesystem::exists(root / ".am/state.sqlite")); clean(root);
  }
}
TEST(CommitProtocol, FallbackOnlyForUnsupportedAndChecksTheMovedIdentity) {
  RecordProperty("req", "ADR-0006|ADR-0025");
#ifdef _WIN32
  const auto root = fresh(); const auto asset = id(); write_version(root / "asset.zip"); asset_zip::ReadBudget budget(1000000);
  auto r = request(root, asset, budget); r.hooks.rename_ex_error = ERROR_NOT_SUPPORTED;
  auto result = commit_zip(r); ASSERT_TRUE(result) << result.error().ctx.detail; EXPECT_EQ(result->replacement, Replacement::move_file_ex); clean(root);
  auto denied = request(root, asset, budget); denied.hooks.rename_ex_error = ERROR_ACCESS_DENIED; bool fallback = false; const auto old = bytes(root / "asset.zip");
  denied.hooks.before_fallback = [&](const Path&) -> Result<void> { fallback = true; return {}; }; result = commit_zip(denied); ASSERT_FALSE(result); EXPECT_FALSE(fallback); EXPECT_EQ(result.error().message_id, "am.commit.replace_failed"); EXPECT_EQ(bytes(root / "asset.zip"), old); clean(root);
  auto swapped = request(root, asset, budget); swapped.hooks.rename_ex_error = ERROR_NOT_SUPPORTED;
  swapped.hooks.before_fallback = [&](const Path& temp) -> Result<void> { write_version(temp, "new", 'q'); return {}; };
  result = commit_zip(swapped); ASSERT_FALSE(result); EXPECT_EQ(result.error().message_id, "am.commit.replace_failed");
  const auto foreign = bytes(root / "asset.zip"); auto report = value(recover(root)); ASSERT_EQ(report.size(), 1u); EXPECT_EQ(report[0].outcome, RecoveryOutcome::rolled_back); EXPECT_EQ(bytes(root / "asset.zip"), foreign); clean(root);
#else
  GTEST_SKIP() << "Win32 fallback test";
#endif
}
TEST(CommitProtocol, ProtectedTemporaryFileDeniesAnotherWriter) {
  RecordProperty("req", "ADR-0006");
#ifdef _WIN32
  const auto root = fresh(); const auto asset = id(); asset_zip::ReadBudget budget(1000000); auto r = request(root, asset, budget); unsigned checks = 0;
  r.hooks.point = [&](FaultPoint p) -> Result<void> {
    if (p == FaultPoint::after_verify || p == FaultPoint::after_verified_journal || p == FaultPoint::before_replace) {
      auto path = am::paths::native(root / ".am/.am-tmp" / (asset + "." + r.op_id + ".zip"));
      auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
      EXPECT_EQ(handle, INVALID_HANDLE_VALUE); if (handle == INVALID_HANDLE_VALUE) EXPECT_EQ(GetLastError(), ERROR_SHARING_VIOLATION); else CloseHandle(handle); ++checks;
    } return {};
  };
  auto result = commit_zip(r); ASSERT_TRUE(result) << result.error().ctx.detail; EXPECT_EQ(checks, 3u); clean(root);
#else
  GTEST_SKIP() << "Win32 sharing test";
#endif
}
TEST(CommitProtocol, LockHeldByAnotherProcessIsReleasedByKill) {
  RecordProperty("req", "ADR-0025");
#ifdef _WIN32
  const auto root = fresh(); const auto asset = id(); write_version(root / "asset.zip"); const auto marker = root / "holder.ready";
  Child holder("hold", root, asset, marker.string()); ready(marker); asset_zip::ReadBudget budget(1000000); auto r = request(root, asset, budget);
  auto blocked = commit_zip(r); ASSERT_FALSE(blocked); EXPECT_EQ(blocked.error().message_id, "am.commit.locked"); EXPECT_FALSE(std::filesystem::exists(root / ".am/state.sqlite"));
  holder.kill(); r.lock_timeout = std::chrono::seconds(2); auto result = commit_zip(r); ASSERT_TRUE(result) << result.error().ctx.detail; clean(root);
#else
  GTEST_SKIP() << "Win32 child process test";
#endif
}
TEST(CommitProtocol, KillEveryCheckpointThreeTimesAndRecover) {
  RecordProperty("req", "ADR-0006|ADR-0025");
#ifdef _WIN32
  for (const auto point : points) for (int repeat = 0; repeat < 3; ++repeat) {
    SCOPED_TRACE(std::string(point_name(point)) + " " + std::to_string(repeat));
    const auto root = fresh(); const auto asset = id(), op = id(); write_version(root / "asset.zip"); const auto old = bytes(root / "asset.zip");
    Child child("crash", root, asset, op, point_name(point)); ASSERT_EQ(child.wait(), 91u); valid(root / "asset.zip");
    RecoveryOptions options; options.lock_timeout = std::chrono::seconds(2); auto report = value(recover(root, options));
    const bool rollback = point == FaultPoint::after_lock || point == FaultPoint::after_prepared || point == FaultPoint::mid_write || point == FaultPoint::after_finish || point == FaultPoint::after_verify;
    if (point == FaultPoint::after_lock) EXPECT_TRUE(report.empty());
    else {
      ASSERT_EQ(report.size(), 1u); const auto expected = rollback ? RecoveryOutcome::rolled_back : point == FaultPoint::after_replace ? RecoveryOutcome::already_committed : point == FaultPoint::after_indexed_journal ? RecoveryOutcome::cleaned : RecoveryOutcome::completed;
      EXPECT_EQ(report[0].outcome, expected); EXPECT_FALSE(report[0].verifier_skipped);
      std::cout << "KILL " << point_name(point) << " outcome=" << outcome_name(report[0].outcome) << " run=" << repeat + 1 << '\n';
    }
    if (point == FaultPoint::after_lock) std::cout << "KILL after_lock outcome=no_pending_operation run=" << repeat + 1 << '\n';
    if (rollback) EXPECT_EQ(bytes(root / "asset.zip"), old); else EXPECT_NE(bytes(root / "asset.zip"), old);
    valid(root / "asset.zip"); clean(root); EXPECT_TRUE(AssetLock::acquire(root, asset, std::chrono::seconds(2)));
  }
#else
  GTEST_SKIP() << "Win32 kill matrix";
#endif
}
TEST(CommitProtocol, RecoveryUsesWholeFileIdentityNotOnlyManifest) {
  RecordProperty("req", "ADR-0006");
#ifdef _WIN32
  for (int mode = 0; mode < 4; ++mode) {
    const auto root = fresh(); const auto asset = id(), op = id(); write_version(root / "asset.zip"); const auto old = bytes(root / "asset.zip");
    const auto point = mode < 2 ? FaultPoint::after_verified_journal : mode == 2 ? FaultPoint::after_replace : FaultPoint::after_committed_journal;
    Child child("crash", root, asset, op, point_name(point)); ASSERT_EQ(child.wait(), 91u);
    const auto path = mode < 2 ? root / ".am/.am-tmp" / (asset + "." + op + ".zip") : root / "asset.zip";
    if (mode == 0) { auto data = bytes(path); data[200] ^= std::byte{1}; std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())); }
    else write_version(path, "new", 'q');
    const auto foreign = mode >= 2 ? bytes(path) : std::vector<std::byte>{}; auto report = value(recover(root)); ASSERT_EQ(report.size(), 1u);
    EXPECT_EQ(report[0].outcome, mode == 3 ? RecoveryOutcome::failed : RecoveryOutcome::rolled_back);
    if (mode == 3) { ASSERT_TRUE(report[0].error); EXPECT_EQ(report[0].error->message_id, "am.commit.base_changed"); }
    EXPECT_FALSE(report[0].verifier_skipped); EXPECT_EQ(bytes(root / "asset.zip"), mode < 2 ? old : foreign); clean(root);
  }
#else
  GTEST_SKIP() << "Win32 crash identity test";
#endif
}
TEST(CommitProtocol, OpenReaderKeepsOldBytesAndNewReadersSeeNewVersion) {
  RecordProperty("req", "ADR-0025");
#ifdef _WIN32
  for (const bool crash : {false, true}) {
    const auto root = fresh(); const auto asset = id(); write_version(root / "asset.zip");
    auto source = value(asset_zip::file_source(root / "asset.zip", true)); auto archive = value(asset_zip::ZipArchive::open(source));
    const auto old = bytes(root / "asset.zip"); asset_zip::ReadBudget budget(1000000); const auto old_hash = value(archive.verify_entry(1, budget, true)).sha256;
    if (crash) { Child child("crash", root, asset, id(), "after_replace"); ASSERT_EQ(child.wait(), 91u); auto report = value(recover(root)); ASSERT_EQ(report.size(), 1u); EXPECT_EQ(report[0].outcome, RecoveryOutcome::already_committed); }
    else { auto r = request(root, asset, budget); auto result = commit_zip(r); ASSERT_TRUE(result) << result.error().ctx.detail; }
    for (const auto& e : archive.entries()) EXPECT_TRUE(archive.verify_entry(e.index, budget, true));
    EXPECT_EQ(value(archive.verify_entry(1, budget, true)).sha256, old_hash); EXPECT_NE(bytes(root / "asset.zip"), old); valid(root / "asset.zip"); clean(root);
  }
  const auto root = fresh(); const auto asset = id(); write_version(root / "asset.zip"); const auto old = bytes(root / "asset.zip");
  auto no_delete = value(asset_zip::file_source(root / "asset.zip")); asset_zip::ReadBudget budget(1000000); auto r = request(root, asset, budget); auto failed = commit_zip(r);
  ASSERT_FALSE(failed); EXPECT_EQ(failed.error().message_id, "am.commit.replace_failed"); EXPECT_EQ(bytes(root / "asset.zip"), old); clean(root);
#else
  GTEST_SKIP() << "Win32 open reader replacement";
#endif
}
TEST(CommitProtocol, ConcurrentReadersNeverObserveAnIntermediateZip) {
  RecordProperty("req", "ADR-0025");
  const auto root = fresh(); const auto asset = id(); write_version(root / "asset.zip"); std::atomic<unsigned> reads = 0, failed = 0;
  std::vector<std::vector<std::byte>> versions{bytes(root / "asset.zip")};
  for (int i = 0; i < 3; ++i) {
    auto sink = std::make_shared<asset_zip::MemorySink>(); auto writer = value(asset_zip::ZipWriter::create(sink));
    produce(writer, "version-" + std::to_string(i), static_cast<char>('a' + i)); value(writer.finish()); versions.emplace_back(sink->bytes().begin(), sink->bytes().end());
  }
  std::jthread observer([&](std::stop_token token) { while (!token.stop_requested()) { try {
    auto source = value(asset_zip::file_source(root / "asset.zip", true)); auto archive = value(asset_zip::ZipArchive::open(source)); asset_zip::ReadBudget budget(1000000);
    for (const auto& e : archive.entries()) value(archive.verify_entry(e.index, budget, true));
    std::vector<std::byte> observed(static_cast<std::size_t>(source->size())); std::size_t offset = 0;
    while (offset < observed.size()) { auto count = value(source->read_at(offset, std::span(observed).subspan(offset))); if (!count) throw std::runtime_error("Intermediate EOF"); offset += count; }
    if (std::find(versions.begin(), versions.end(), observed) == versions.end()) throw std::runtime_error("Intermediate ZIP bytes");
    ++reads;
  } catch (...) { ++failed; } } });
  for (int i = 0; i < 3; ++i) {
    asset_zip::ReadBudget budget(1000000); auto r = request(root, asset, budget); r.hooks.point = [](FaultPoint p) -> Result<void> { if (p == FaultPoint::before_replace) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return {}; };
    r.producer = [i](auto& writer) -> Result<void> { produce(writer, "version-" + std::to_string(i), static_cast<char>('a' + i)); return {}; };
    auto result = commit_zip(r); EXPECT_TRUE(result) << (result ? "" : result.error().ctx.detail);
  }
  observer.request_stop(); observer.join(); EXPECT_GT(reads, 0u); EXPECT_EQ(failed, 0u); std::cout << "ATOMIC observations=" << reads << " invalid=" << failed << '\n'; clean(root);
}
TEST(CommitProtocol, OrphanAgeAndBusyHandleProtectCleanup) {
  RecordProperty("req", "ADR-0006");
  const auto root = fresh(); const auto dir = root / ".am/.am-tmp"; std::filesystem::create_directories(dir);
  write_version(dir / "young.zip"); write_version(dir / "old.zip"); write_version(dir / "busy.zip");
  const auto old_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(25); std::filesystem::last_write_time(dir / "old.zip", old_time); std::filesystem::last_write_time(dir / "busy.zip", old_time);
  auto held = value(asset_zip::file_source(dir / "busy.zip")); auto report = value(recover(root));
  EXPECT_TRUE(std::filesystem::exists(dir / "young.zip")); EXPECT_FALSE(std::filesystem::exists(dir / "old.zip")); EXPECT_TRUE(std::filesystem::exists(dir / "busy.zip"));
  EXPECT_EQ(std::count_if(report.begin(), report.end(), [](const auto& x) { return x.outcome == RecoveryOutcome::cleaned; }), 1);
}
TEST(CommitProtocol, MixedRecoveryOperationsAndBusyAssetAreIndependent) {
  RecordProperty("req", "ADR-0006|ADR-0025");
  const auto root = fresh(); auto journal = value(db::StateJournal::open(root)); std::vector<std::string> assets;
  for (int n = 0; n < 4; ++n) {
    db::PendingOperation row; row.op_id = id(); row.asset_id = id(); row.kind = "create"; assets.push_back(row.asset_id); row.target_path = root / (std::to_string(n) + ".zip"); row.temp_path = root / ".am/.am-tmp" / (row.asset_id + "." + row.op_id + ".zip");
    std::filesystem::create_directories(row.temp_path.parent_path()); write_version(row.temp_path); value(journal->insert(row));
    if (n == 1 || n == 2) { db::NewFileEvidence evidence{0, std::string(64, 'a'), std::string(64, 'b')}; value(journal->advance(row.op_id, db::CommitState::prepared, db::CommitState::verified, evidence)); value(journal->advance(row.op_id, db::CommitState::verified, db::CommitState::committed)); if (n == 2) value(journal->advance(row.op_id, db::CommitState::committed, db::CommitState::indexed)); std::filesystem::remove(row.temp_path); }
  }
  auto held = value(AssetLock::acquire(root, assets[3])); auto report = value(recover(root));
  EXPECT_EQ(std::count_if(report.begin(), report.end(), [](const auto& x) { return x.outcome == RecoveryOutcome::skipped_locked; }), 1);
  EXPECT_EQ(value(journal->list()).size(), 1u);
}
TEST(CommitProtocol, RawCopyPreservesDosTimeAndNewEntryOptionsStillApply) {
  RecordProperty("req", "ADR-0006");
  auto a = std::make_shared<asset_zip::MemorySink>(); asset_zip::WriterOptions o; o.dos_time = {0x5432, 0x3456}; auto w = value(asset_zip::ZipWriter::create(a, o)); add(w, "manifest.json", manifest("old")); auto summary = value(w.finish());
  auto source = value(asset_zip::memory_source({a->bytes().begin(), a->bytes().end()})); auto archive = value(asset_zip::ZipArchive::open(source));
  auto b = std::make_shared<asset_zip::MemorySink>(); o.dos_time = {0x2222, 0x1111}; auto out = value(asset_zip::ZipWriter::create(b, o)); asset_zip::ReadBudget budget(1000000); value(out.copy_raw(archive, 0, budget));
  asset_zip::EntryOptions entry; entry.dos_time = asset_zip::DosTime{0x3333, 0x1234}; auto e = value(out.begin_entry("content/empty", entry)); value(e.finish()); value(out.finish());
  auto copy = value(asset_zip::ZipArchive::open(value(asset_zip::memory_source({b->bytes().begin(), b->bytes().end()}))));
  EXPECT_EQ(copy.entries()[0].dos_time, 0x3456); EXPECT_EQ(copy.entries()[0].dos_date, 0x5432); EXPECT_EQ(copy.entries()[1].dos_time, 0x1234); EXPECT_EQ(copy.entries()[1].dos_date, 0x3333);
}
TEST(CommitProtocol, OneGiBVerificationMemoryAndWholeFileHashTiming) {
  RecordProperty("req", "K-116|ADR-0006");
  const auto root = fresh(); asset_zip::ReadBudget budget(2ULL * 1024 * 1024 * 1024); auto r = request(root, id(), budget); r.estimated_size = 16 * 1024 * 1024;
  r.producer = [](auto& writer) -> Result<void> {
    add(writer, "manifest.json", manifest("new")); asset_zip::EntryOptions o; o.uncompressed_size = 1024ULL * 1024 * 1024;
    auto entry = value(writer.begin_entry("content/zeros", o)); std::array<std::byte, 65536> zeros{};
    for (std::uint64_t n = 0; n < o.uncompressed_size; n += zeros.size()) value(entry.write(zeros)); value(entry.finish()); return {};
  };
  const auto started = std::chrono::steady_clock::now(); auto result = commit_zip(r); ASSERT_TRUE(result) << result.error().ctx.detail;
  std::cout << "COMMIT 1GiB: seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " file_hash_seconds=" << result->file_hash_seconds << " zip_bytes=" << result->size;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory); ASSERT_TRUE(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)));
  std::cout << " peak_working_set_bytes=" << memory.PeakWorkingSetSize << " peak_commit_bytes=" << memory.PeakPagefileUsage;
#endif
  std::cout << '\n'; clean(root);
}
TEST(CommitProtocol, WindowsAndFilesystemEvidence) {
  RecordProperty("req", "ADR-0025");
#ifdef _WIN32
  const auto root = fresh(); std::array<wchar_t, 32768> volume{}; std::array<wchar_t, 64> filesystem{};
  ASSERT_TRUE(GetVolumePathNameW(root.c_str(), volume.data(), static_cast<DWORD>(volume.size()))); ASSERT_TRUE(GetVolumeInformationW(volume.data(), nullptr, 0, nullptr, nullptr, nullptr, filesystem.data(), static_cast<DWORD>(filesystem.size())));
  EXPECT_EQ(std::wstring(filesystem.data()), L"NTFS"); OSVERSIONINFOW version{}; version.dwOSVersionInfoSize = sizeof(version);
  using Version = LONG(WINAPI*)(OSVERSIONINFOW*); Version query = nullptr; auto address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"); std::memcpy(&query, &address, sizeof(query)); ASSERT_NE(query, nullptr); ASSERT_EQ(query(&version), 0);
  std::wcout << L"ENV Windows=" << version.dwMajorVersion << L'.' << version.dwMinorVersion << L" build=" << version.dwBuildNumber << L" filesystem=" << filesystem.data() << L" volume=" << volume.data() << L'\n';
#else
  GTEST_SKIP() << "Win32 OS and NTFS evidence APIs";
#endif
}
