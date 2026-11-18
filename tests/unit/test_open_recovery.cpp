#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>
#include <thread>
#include "am/application/library_service.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/catalog/indexer.hpp"
#include "am/storage/db/job_store.hpp"
#include "am/storage/db/operation_log.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace cat = am::catalog;
namespace lib = am::library;
namespace repo = am::repository;
namespace commit = am::commit;
namespace zip = am::asset_zip;
using Json = repo::Json;
template<class T> T value(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(r).value();
}
[[maybe_unused]] void value(am::Result<void> r) { if (!r) throw std::runtime_error(am::to_log_string(r.error())); }
[[maybe_unused]] std::string id() { return am::Uuid::v4().to_string(); }
void write(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path()); std::ofstream f(am::paths::native(p),std::ios::binary); f << s;
  if (!f) throw std::runtime_error("Synthetic fixture write failed");
}
lib::OpenOptions options(lib::Mode mode = lib::Mode::read_write) {
  lib::OpenOptions o; o.mode = mode; o.application_version = "test"; o.capabilities.emplace(); o.capabilities->mode = repo::StorageMode::full; return o;
}
[[maybe_unused]] std::string path_text(const fs::path& p) { const auto s = p.generic_u8string(); return {s.begin(),s.end()}; }
[[maybe_unused]] fs::path asset_path(const fs::path& root, const std::string& asset) { return root/"assets"/asset.substr(34)/(asset+".zip"); }
}

#ifdef AM_OPEN_RECOVERY_HELPER
int main(int argc, char** argv) {
  if (argc != 8) return 90;
  try {
    const fs::path root(argv[1]); const std::string library_id = argv[2], asset = argv[3], kind = argv[4], point = argv[5];
    if (kind == "hold") {
      auto lock = value(commit::AssetLock::acquire(root,asset)); write(fs::path(argv[6]),"ready");
      for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto catalog = value(cat::Catalog::open(root,options()));
    repo::RepositoryOptions o; o.library_id = library_id; o.capabilities = options().capabilities;
    o.test_hooks.commit.mid_write_after_bytes = 1;
    o.test_hooks.commit.point = [&](commit::FaultPoint p) -> am::Result<void> {
      if (commit::point_name(p) == point) std::_Exit(91);
      return {};
    };
    auto repository = value(repo::AssetRepository::open(root,o)); const repo::AssetRef ref{library_id,asset};
    if (kind == "change") {
      const auto snapshot = value(repository.snapshot(ref)); repo::ChangeRequest r; r.ref = ref; r.expected_rev = snapshot.revision(); r.op_id = argv[7];
      repo::SetMetadata m; m.field = "title"; m.value.emplace("Recovered title"); r.ops.emplace_back(m); value(repository.change(r));
    } else if (kind == "create") {
      repo::CreateRequest r; r.asset_id = asset; r.op_id = argv[7]; r.metadata = Json{{"title","Recovered creation"}}; value(repository.create(r));
    } else if (kind == "trash") value(repository.trash(ref));
    else if (kind == "import_ready") value(repository.import_ready({fs::path(argv[6]),{}}));
    else return 92;
    return 93;
  } catch (...) { return 94; }
}
#else
#include <gtest/gtest.h>
#include "zip_builder.hpp"
namespace {
std::string bytes(const fs::path& p) { std::ifstream f(am::paths::native(p),std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
std::string hash_text(std::string_view s) {
  auto h = value(zip::Sha256::create()); value(h.update(std::as_bytes(std::span(s))));
  const auto digest = value(h.finish()); std::string out; constexpr char hex[] = "0123456789abcdef";
  for (auto b : digest) { const auto n = std::to_integer<unsigned>(b); out += hex[n>>4]; out += hex[n&15]; } return out;
}
std::string hash(const fs::path& p) { return hash_text(bytes(p)); }
fs::path fresh() {
  const auto root = fs::path(AM_LIBRARY_TEST_ROOT)/id(); fs::create_directories(root.parent_path()); value(lib::Library::init(root)); return root;
}
std::string scalar(lib::Library& l, const std::string& sql) {
  auto c = am::db::Connection::borrow(l.database(lib::DatabaseKind::index)); auto q = value(c.prepare(sql));
  value(q.step()); return value(q.text(0));
}
struct World {
  fs::path root = fresh();
  std::string library_id;
  repo::ChangeResult asset;
  explicit World(const std::optional<std::string>& identity = {}) {
    auto c = value(cat::Catalog::open(root,options())); library_id = c.library().config().library_id;
    value(c.indexer().rebuild()); asset = add(c,identity);
  }
  repo::ChangeResult add(cat::Catalog& c, const std::optional<std::string>& identity = {}) {
    repo::CreateRequest r; r.asset_id = identity; r.metadata = Json{{"title","Initial title"}};
    repo::AddFiles a; a.input = {{"id",id()},{"mode","strict"},{"imported_at","2026-11-17T00:00:00.000Z"},
      {"completeness","complete"},{"objects",Json::array()},{"entries",Json::array()}};
    repo::FileInput f; f.path = "payload.bin"; f.source = root/(id()+".bin"); write(f.source,"payload"); a.files.push_back(f); r.files.push_back(a);
    return value(c.create(r)).change;
  }
  fs::path path() const { return asset_path(root,asset.ref.asset_id); }
  std::vector<am::db::PendingOperation> pending() { return value(value(am::db::StateJournal::open(root))->list()); }
  void clean() { EXPECT_TRUE(pending().empty()); EXPECT_TRUE(fs::is_empty(root/".am/.am-tmp")); }
  void install(const fs::path& path, const Json& j, bool damaged = false, bool invalid_manifest = false) {
    zip_test::Entry m; m.name = "manifest.json"; m.data = invalid_manifest ? "{" : j.dump();
    zip_test::Entry payload; payload.name = j["files"][0]["storage_path"].get<std::string>();
    // Stored streams have the same size after replacement; the independent ZIP
    // builder regenerates CRC. Only the manifest SHA-256 then fails at V4.
    payload.data = damaged ? "PAYLOAD" : "payload";
    const auto built = zip_test::build({m,payload});
    write(path,std::string(reinterpret_cast<const char*>(built.bytes.data()),built.bytes.size()));
  }
  Json plain() { auto c = value(cat::Catalog::open(root,options(lib::Mode::read_only))); return Json::parse(value(c.library().assets().snapshot(asset.ref)).manifest_text()); }
};
class Worker {
#ifdef _WIN32
  HANDLE process_ = nullptr;
#else
  pid_t process_ = -1;

#endif
 public:
  Worker(const World& w, const std::string& asset, const std::string& kind, const std::string& point, const std::string& input = "-", const std::string& operation = "-") {
    const std::vector<std::string> args{AM_OPEN_HELPER_PATH,path_text(w.root),w.library_id,asset,kind,point,input,operation};
#ifdef _WIN32
    std::wstring command;
    for (const auto& arg : args) { if (!command.empty()) command += L' '; command += L'"'; command += fs::path(std::u8string(arg.begin(),arg.end())).wstring(); command += L'"'; }
    STARTUPINFOW start{}; start.cb = sizeof(start); PROCESS_INFORMATION p{};
    if (!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&p)) throw std::runtime_error("Worker launch failed");
    process_ = p.hProcess; CloseHandle(p.hThread);
#else
    std::vector<char*> raw; for (const auto& arg : args) raw.push_back(const_cast<char*>(arg.c_str())); raw.push_back(nullptr);
    process_ = fork(); if (process_ < 0) throw std::runtime_error("Worker fork failed");
    if (process_ == 0) { execv(raw[0],raw.data()); std::_Exit(95); }
#endif
  }
  int wait() {
#ifdef _WIN32
    if (WaitForSingleObject(process_,30000) != WAIT_OBJECT_0) throw std::runtime_error("Worker timed out");
    DWORD result = 0; GetExitCodeProcess(process_,&result); CloseHandle(process_); process_ = nullptr; return static_cast<int>(result);
#else
    const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(30); int status = 0;
    while (waitpid(process_,&status,WNOHANG) == 0) {
      if (std::chrono::steady_clock::now() > until) throw std::runtime_error("Worker timed out");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    process_ = -1; return WIFEXITED(status) ? WEXITSTATUS(status) : 96;
#endif
  }
  void stop() noexcept {
#ifdef _WIN32
    if (process_) { TerminateProcess(process_,92); WaitForSingleObject(process_,30000); CloseHandle(process_); process_ = nullptr; }
#else
    if (process_ > 0) { kill(process_,SIGKILL); int status = 0; waitpid(process_,&status,0); process_ = -1; }
#endif
  }
  ~Worker() { stop(); }
};
void crash(World& w, const std::string& kind, const std::string& point, const std::string& asset = {}, const std::string& input = "-") {
  Worker child(w,asset.empty() ? w.asset.ref.asset_id : asset,kind,point,input,id()); ASSERT_EQ(child.wait(),91);
}
bool warning(const cat::OpenReport& report, const std::string& message) {
  return std::any_of(report.warnings.begin(),report.warnings.end(),[&](const auto& e) { return e.message_id == message; });
}
}

TEST(OpenRecovery, CrashedChangeAtEveryFaultPointOpensWithoutExplicitRecovery) {
  RecordProperty("req","K-122|K-21");
  for (auto p : {"after_lock","after_prepared","mid_write","after_finish","after_verify","after_verified_journal","before_replace","after_replace","after_committed_journal","after_indexed_journal"}) {
    SCOPED_TRACE(p); World w; const auto original = hash(w.path()); crash(w,"change",p); const auto before = w.pending();
    auto c = value(cat::Catalog::open(w.root,options()));
    if (std::string_view(p) == "after_lock") { EXPECT_TRUE(before.empty()); EXPECT_TRUE(c.open_report().recovered.empty()); EXPECT_EQ(hash(w.path()),original); }
    else { ASSERT_EQ(before.size(),1u); ASSERT_EQ(c.open_report().recovered.size(),1u); EXPECT_EQ(c.open_report().recovered[0].op_id,before[0].op_id); }
    const auto snap = value(c.library().assets().snapshot(w.asset.ref));
    EXPECT_FALSE(value(c.library().assets().verify(w.asset.ref)).error);
    EXPECT_EQ(scalar(c.library(),"SELECT revision FROM assets"),std::to_string(snap.revision()));
    EXPECT_EQ(scalar(c.library(),"SELECT manifest_sha256 FROM assets"),hash_text(snap.manifest_text()));
    EXPECT_EQ(scalar(c.library(),"SELECT manifest_json FROM assets"),snap.manifest_text());
    EXPECT_EQ(scalar(c.library(),"SELECT in_trash FROM assets"),"0"); w.clean();
  }
}
TEST(OpenRecovery, CrashedCreateTrashAndImportAreReindexed) {
  RecordProperty("req","K-122|K-21");
  for (const auto* kind : {"create","trash","import_ready"}) for (const auto* point : {"after_prepared","after_replace","after_indexed_journal"}) {
    SCOPED_TRACE(std::string(kind)+":"+point); World w, external;
    const auto target = std::string_view(kind) == "create" ? id() : std::string_view(kind) == "import_ready" ? external.asset.ref.asset_id : w.asset.ref.asset_id;
    crash(w,kind,point,target,path_text(external.path())); const auto before = w.pending(); ASSERT_EQ(before.size(),1u);
    auto c = value(cat::Catalog::open(w.root,options())); ASSERT_EQ(c.open_report().recovered.size(),1u);
    EXPECT_EQ(c.open_report().recovered[0].op_id,before[0].op_id);
    const bool active = fs::exists(asset_path(w.root,target)), trash = fs::exists(w.root/"trash"/(target+".zip"));
    const auto rows = value(c.indexer().list(true)); auto found = std::find_if(rows.begin(),rows.end(),[&](const auto& r) { return r.asset_id == target; });
    EXPECT_EQ(found != rows.end(),active || trash);
    if (found != rows.end()) { EXPECT_EQ(found->in_trash,trash); EXPECT_EQ(found->revision,1); }
    w.clean();
  }
}
TEST(OpenRecovery, UnclosedSessionSyncsLostNotificationClosedSessionDoesNotSync) {
  RecordProperty("req","K-24"); World w;
  { auto l = value(lib::Library::open(w.root,options())); am::db::IndexStore store(l.database(lib::DatabaseKind::index)); value(store.set_meta("writer_session","open"));
    repo::ChangeRequest change; change.ref = w.asset.ref; change.expected_rev = 1; repo::SetMetadata m; m.field = "title"; m.value.emplace("Unindexed change"); change.ops.emplace_back(m);
    value(l.assets().change(change)); EXPECT_TRUE(w.pending().empty()); EXPECT_EQ(scalar(l,"SELECT revision FROM assets"),"1"); }
  { auto c = value(cat::Catalog::open(w.root,options())); EXPECT_TRUE(c.open_report().previous_session_unclosed); ASSERT_TRUE(c.open_report().sync); EXPECT_EQ(scalar(c.library(),"SELECT revision FROM assets"),"2"); }
  { auto c = value(cat::Catalog::open(w.root,options())); EXPECT_FALSE(c.open_report().previous_session_unclosed); EXPECT_FALSE(c.open_report().sync); EXPECT_EQ(scalar(c.library(),"SELECT value FROM meta WHERE key='writer_session'"),"open"); }
  { auto l = value(lib::Library::open(w.root,options())); EXPECT_EQ(scalar(l,"SELECT value FROM meta WHERE key='writer_session'"),"closed"); auto db = am::db::Connection::borrow(l.database(lib::DatabaseKind::index)); value(db.exec("DELETE FROM meta WHERE key='writer_session'")); }
  { auto c = value(cat::Catalog::open(w.root,options())); EXPECT_TRUE(c.open_report().previous_session_unclosed); EXPECT_TRUE(c.open_report().sync); }
  { auto c = value(cat::Catalog::open(w.root,options())); EXPECT_FALSE(c.open_report().sync); }
}
TEST(OpenRecovery, ReadOnlyReportsPendingWithoutChangingFiles) {
  RecordProperty("req","K-122"); World w; crash(w,"change","after_verified_journal"); const auto pending = w.pending(); ASSERT_EQ(pending.size(),1u);
  const auto source = hash(w.path()), temp = hash(pending[0].temp_path), state = hash(w.root/".am/state.sqlite"), index = hash(w.root/".am/index.sqlite");
  { auto c = value(cat::Catalog::open(w.root,options(lib::Mode::read_only))); EXPECT_EQ(c.open_report().pending_read_only,1u); EXPECT_TRUE(warning(c.open_report(),"am.library.recovery_pending")); EXPECT_TRUE(c.open_report().recovered.empty()); }
  EXPECT_EQ(hash(w.path()),source); EXPECT_EQ(hash(pending[0].temp_path),temp); EXPECT_EQ(hash(w.root/".am/state.sqlite"),state); EXPECT_EQ(hash(w.root/".am/index.sqlite"),index);
}
TEST(OpenRecovery, BrokenJournalFailsBeforeRecoveryAndReadOnlyRemainsAvailable) {
  RecordProperty("req","K-122"); World w;
  { auto l = value(lib::Library::open(w.root,options())); auto db = am::db::Connection::borrow(l.database(lib::DatabaseKind::state)); value(db.exec("ALTER TABLE pending_ops RENAME COLUMN op_id TO broken_id")); }
  const auto source = hash(w.path()), state = hash(w.root/".am/state.sqlite"), index = hash(w.root/".am/index.sqlite");
  const auto failed = cat::Catalog::open(w.root,options()); ASSERT_FALSE(failed); EXPECT_EQ(failed.error().message_id,"am.library.recovery_failed"); EXPECT_TRUE(failed.error().cause);
  EXPECT_EQ(hash(w.path()),source); EXPECT_EQ(hash(w.root/".am/state.sqlite"),state); EXPECT_EQ(hash(w.root/".am/index.sqlite"),index);
  EXPECT_TRUE(cat::Catalog::open(w.root,options(lib::Mode::read_only)));
}
TEST(OpenRecovery, LockedItemWarnsAndNextOpenFinishesIt) {
  RecordProperty("req","K-122"); World w; crash(w,"change","after_verified_journal"); const auto pending = w.pending(); ASSERT_EQ(pending.size(),1u);
  const auto marker = w.root/"holder-ready"; Worker holder(w,w.asset.ref.asset_id,"hold","-",path_text(marker));
  const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(30);
  while (!fs::exists(marker)) { ASSERT_LT(std::chrono::steady_clock::now(),until); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
  { auto c = value(cat::Catalog::open(w.root,options())); ASSERT_EQ(c.open_report().recovered.size(),1u); EXPECT_EQ(c.open_report().recovered[0].outcome,commit::RecoveryOutcome::skipped_locked); EXPECT_TRUE(warning(c.open_report(),"am.library.recovery_incomplete")); EXPECT_EQ(w.pending().size(),1u); }
  holder.stop(); auto c = value(cat::Catalog::open(w.root,options())); EXPECT_EQ(value(c.indexer().list())[0].revision,2); w.clean();
}
TEST(OpenRecovery, RefreshHandlesActiveTrashAndMissingAndIndexFailureWarns) {
  RecordProperty("req","K-21"); World w;
  { auto c = value(cat::Catalog::open(w.root,options())); value(c.indexer().refresh(w.asset.ref.asset_id)); EXPECT_FALSE(value(c.indexer().list())[0].in_trash);
    value(c.trash(w.asset.ref)); value(c.indexer().refresh(w.asset.ref.asset_id)); EXPECT_TRUE(value(c.indexer().list(true))[0].in_trash);
    value(c.purge(w.asset.ref)); value(c.indexer().refresh(w.asset.ref.asset_id)); EXPECT_TRUE(value(c.indexer().list(true)).empty()); }
  World damaged; crash(damaged,"change","after_replace"); cat::IndexerHooks hooks;
  hooks.before_update = [](auto) -> am::Result<void> { return am::Error::make(am::Errc::io_error,"Injected index failure"); };
  auto c = value(cat::Catalog::open(damaged.root,options(),hooks)); EXPECT_TRUE(warning(c.open_report(),"am.catalog.index_update_failed")); EXPECT_TRUE(value(c.indexer().rebuild_required())); EXPECT_EQ(value(c.library().assets().snapshot(damaged.asset.ref)).revision(),2u); damaged.clean();
}
TEST(LibraryVerify, QuickFullTrashDamageAndStickyIndexHealth) {
  RecordProperty("req","K-22"); World w; std::vector<repo::ChangeResult> assets{w.asset}; std::vector<Json> manifests;
  { auto c = value(cat::Catalog::open(w.root,options())); for (int i = 0; i < 4; ++i) assets.push_back(w.add(c));
    for (const auto& a : assets) manifests.push_back(Json::parse(value(c.library().assets().snapshot(a.ref)).manifest_text()));
    value(c.trash(assets[4].ref)); }
  w.install(asset_path(w.root,assets[1].ref.asset_id),manifests[1],true);
  w.install(asset_path(w.root,assets[2].ref.asset_id),manifests[2],false,true);
  manifests[3]["format"]["version"]["minor"] = 1; w.install(asset_path(w.root,assets[3].ref.asset_id),manifests[3]);
  std::map<fs::path,std::string> hashes;
  for (std::size_t i = 0; i < assets.size(); ++i) { const auto p = i == 4 ? w.root/"trash"/(assets[i].ref.asset_id+".zip") : asset_path(w.root,assets[i].ref.asset_id); hashes[p] = hash(p); }
  auto c = value(cat::Catalog::open(w.root,options())); const auto quick = value(c.verify());
  EXPECT_EQ(quick.total,5u); EXPECT_EQ(quick.ok,3u); EXPECT_EQ(quick.warn,1u); EXPECT_EQ(quick.corrupt,1u);
  cat::LibraryVerifyOptions full; full.level = cat::LibraryVerifyLevel::full; const auto checked = value(c.verify(full));
  EXPECT_EQ(checked.ok,2u); EXPECT_EQ(checked.corrupt,2u); EXPECT_EQ(checked.warn,1u); EXPECT_TRUE(checked.index_updated);
  const auto bad = std::find_if(checked.items.begin(),checked.items.end(),[&](const auto& item) { return item.asset_id == assets[1].ref.asset_id; });
  ASSERT_NE(bad,checked.items.end()); ASSERT_TRUE(bad->stream); EXPECT_EQ(bad->status,"corrupt"); EXPECT_FALSE(bad->message_id.empty());
  value(c.sync()); EXPECT_EQ(scalar(c.library(),"SELECT health FROM assets WHERE asset_id='"+assets[1].ref.asset_id+"'"),"corrupt");
  full.include_trash = false; const auto active = value(c.verify(full)); EXPECT_EQ(active.total,4u);
  EXPECT_TRUE(std::none_of(active.items.begin(),active.items.end(),[](const auto& item) { return item.in_trash; }));
  for (const auto& [p,digest] : hashes) EXPECT_EQ(hash(p),digest);
  w.install(asset_path(w.root,assets[1].ref.asset_id),manifests[1]); value(c.verify(full));
  EXPECT_EQ(scalar(c.library(),"SELECT health FROM assets WHERE asset_id='"+assets[1].ref.asset_id+"'"),"ok");
  auto external = manifests[0]; const auto external_id = id(); external["asset"]["id"] = external_id; w.install(asset_path(w.root,external_id),external);
  const auto count = value(c.indexer().count()); const auto unindexed = value(c.verify(full));
  auto found = std::find_if(unindexed.items.begin(),unindexed.items.end(),[&](const auto& item) { return item.asset_id == external_id; });
  ASSERT_NE(found,unindexed.items.end()); EXPECT_FALSE(found->indexed); EXPECT_EQ(found->status,"ok"); EXPECT_EQ(value(c.indexer().count()),count);
}
TEST(LibraryVerify, ReadOnlyAndMissingIndexDoNotWrite) {
  RecordProperty("req","K-22"); World w; const auto index = hash(w.root/".am/index.sqlite"), asset = hash(w.path());
  { auto c = value(cat::Catalog::open(w.root,options(lib::Mode::read_only))); cat::LibraryVerifyOptions o; o.level = cat::LibraryVerifyLevel::full;
    const auto report = value(c.verify(o)); EXPECT_EQ(report.ok,1u); EXPECT_FALSE(report.index_updated); }
  EXPECT_EQ(hash(w.root/".am/index.sqlite"),index); EXPECT_EQ(hash(w.path()),asset);
  ASSERT_TRUE(fs::remove(w.root/".am/index.sqlite"));
  { auto c = value(cat::Catalog::open(w.root,options(lib::Mode::read_only))); const auto report = value(c.verify()); EXPECT_EQ(report.ok,1u); EXPECT_FALSE(report.items[0].indexed); EXPECT_FALSE(report.index_updated); }
  EXPECT_FALSE(fs::exists(w.root/".am/index.sqlite"));
}
TEST(LibraryVerify, ProgressCancellationAppliesOnlyCheckedAssets) {
  RecordProperty("req","K-22"); World w{std::string("ffffffff-ffff-4fff-bfff-fffffffffffe")}; std::vector<repo::ChangeResult> assets;
  { auto c = value(cat::Catalog::open(w.root,options()));
    for (auto x : {"00000000-0000-4000-8000-000000000001","00000000-0000-4000-8000-000000000002"}) assets.push_back(w.add(c,std::string(x)));
    for (const auto& a : assets) {
      const auto manifest = Json::parse(value(c.library().assets().snapshot(a.ref)).manifest_text());
      w.install(asset_path(w.root,a.ref.asset_id),manifest,true);
    } }
  auto c = value(cat::Catalog::open(w.root,options())); std::vector<std::size_t> progress;
  cat::LibraryVerifyOptions o; o.level = cat::LibraryVerifyLevel::full;
  o.progress = [&](auto done,auto total) { EXPECT_EQ(total,3u); progress.push_back(done); };
  o.cancelled = [&] { return !progress.empty() && progress.back() == 2; };
  const auto report = value(c.verify(o)); EXPECT_TRUE(report.cancelled); EXPECT_EQ(report.checked,2u); EXPECT_EQ(report.corrupt,2u); EXPECT_TRUE(report.index_updated);
  EXPECT_TRUE(std::is_sorted(progress.begin(),progress.end())); EXPECT_EQ(progress,(std::vector<std::size_t>{0,1,2}));
  for (const auto& a : assets) EXPECT_EQ(scalar(c.library(),"SELECT health FROM assets WHERE asset_id='"+a.ref.asset_id+"'"),"corrupt");
  EXPECT_EQ(scalar(c.library(),"SELECT health FROM assets WHERE asset_id='"+w.asset.ref.asset_id+"'"),"ok");
}
TEST(LibraryVerify, UnsupportedUnreadableAndSemanticPointerAreReported) {
  RecordProperty("req","K-22"); World w; auto j = w.plain();
  j["groups"] = Json::array({{{"id",id()},{"type","custom"},{"name","Broken group"},{"file_refs",Json::array({id()})}}}); w.install(w.path(),j);
  { auto c = value(cat::Catalog::open(w.root,options())); const auto r = value(c.verify()); ASSERT_EQ(r.corrupt,1u); ASSERT_TRUE(r.items[0].pointer); EXPECT_EQ(*r.items[0].pointer,"/groups/0/file_refs/0"); }
  j.erase("groups"); j["format"]["version"]["major"] = 2; w.install(w.path(),j);
  auto c = value(cat::Catalog::open(w.root,options())); auto other = w.add(c); const auto other_path = asset_path(w.root,other.ref.asset_id), held = w.root/"held.zip";
  const auto digest = hash(other_path); cat::LibraryVerifyOptions o;
  o.progress = [&](auto done,auto) { if (!done) fs::rename(other_path,held); };
  const auto r = value(c.verify(o)); EXPECT_EQ(r.unsupported,1u); EXPECT_EQ(r.unreadable,1u); EXPECT_EQ(hash(held),digest);
}
TEST(LibraryVerify, RepairTransactionRollsBackTogether) {
  RecordProperty("req","K-22"); World w; bool repairing = false; int repaired = 0; cat::IndexerHooks hooks;
  hooks.before_update = [&](auto) -> am::Result<void> {
    if (repairing && ++repaired == 2) return am::Error::make(am::Errc::io_error,"Injected second repair failure");
    return {};
  };
  auto c = value(cat::Catalog::open(w.root,options(),hooks)); auto other = w.add(c);
  std::vector<std::pair<fs::path,Json>> originals;
  for (const auto& a : {w.asset,other}) { const auto p = asset_path(w.root,a.ref.asset_id); originals.emplace_back(p,Json::parse(value(c.library().assets().snapshot(a.ref)).manifest_text())); w.install(p,originals.back().second,true); }
  cat::LibraryVerifyOptions full; full.level = cat::LibraryVerifyLevel::full; EXPECT_EQ(value(c.verify(full)).corrupt,2u);
  for (const auto& [p,j] : originals) w.install(p,j);
  repairing = true; const auto report = value(c.verify(full)); EXPECT_FALSE(report.index_updated); EXPECT_EQ(repaired,2); EXPECT_TRUE(value(c.indexer().rebuild_required()));
  for (const auto& a : {w.asset,other}) EXPECT_EQ(scalar(c.library(),"SELECT health FROM assets WHERE asset_id='"+a.ref.asset_id+"'"),"corrupt");
}
TEST(SelfSufficiency, ReadyImportAndDiskRebuildNeedOnlyTheZip) {
  RecordProperty("req","K-23|K-31"); World source; auto j = source.plain();
  const auto file = j["files"][0]["id"];
  const auto group = id(), child = id(), preview = id(), author = id(), origin = id(), object = id(), entry = id();
  const auto field = "user:"+id();
  j["asset"]["metadata"] = {{"title","Portable model"},{"description","Portable description"},{"tags",Json::array({"Portable","tag"})},
    {"categories",Json::array({{{"path",Json::array({"Models","Test"})}}})},{"authors",Json::array({{{"id",author},{"name","Test author"}}})},
    {"license",{{"name","Test license"},{"file_ref",file}}},{"custom_values",Json::array({{{"field_id",field},{"value",true}}})}};
  j["field_definitions"] = Json::array({{{"id",field},{"schema_revision",1},{"name","Portable field"},{"type","boolean"},{"scopes",Json::array({"asset"})}}});
  j["files"][0]["origin_kind"] = "imported"; j["files"][0]["type"] = {{"id","mesh.stl.binary"},{"by","user"}};
  j["groups"] = Json::array({{{"id",group},{"name","Root"},{"type","custom"},{"file_refs",Json::array({file})}},
    {{"id",child},{"name","Child"},{"type","custom"},{"parent_id",group},{"file_refs",Json::array()}}});
  j["previews"] = {{"primary_id",preview},{"items",Json::array({{{"id",preview},{"file_id",file},{"group_refs",Json::array({child})}}})}};
  j["origin"]["sources"] = Json::array({{{"id",origin},{"kind","test"},{"name","Portable origin"},{"url","https://example.com/model"}}});
  auto& input = j["inputs"][0]; input["source_refs"] = Json::array({origin});
  input["objects"] = Json::array({{{"id",object},{"kind","archive"},{"name","source.zip"},{"container","zip"}}});
  input["entries"] = Json::array({{{"id",entry},{"object_id",object},{"entry_kind","file"},{"path","payload.bin"},{"result","included"},{"file_id",file},{"size_bytes","7"}}});
  source.install(source.path(),j);
  const auto comparable = [](lib::Library& l) {
    std::map<std::string,std::vector<std::string>> result;
    const std::map<std::string,std::string> columns{
      {"assets","asset_id,revision,manifest_sha256,title,file_count,content_bytes,formats,description,primary_preview"},
      {"files","asset_id,file_id,storage_path,name,size,sha256,origin_kind,type_id"},
      {"sources","asset_id,source_id,kind,name,url"},{"asset_tags","asset_id,tag"},{"asset_categories","asset_id,path"},
      {"asset_fields","asset_id,scope,owner_id,field_id,type_id,num,txt,unit"},
      {"input_entries","asset_id,entry_id,input_id,object_id,src_path,src_name,entry_kind,size,crc32,result,file_id"},
      {"input_objects","asset_id,object_id,input_id,kind,volume_index,original_name,size,sha256"}};
    auto db = am::db::Connection::borrow(l.database(lib::DatabaseKind::index));
    for (const auto& [table,fields] : columns) {
      auto q = value(db.prepare("SELECT json_array("+fields+") FROM "+table));
      while (value(q.step()) == am::db::Step::row) result[table].push_back(value(q.text(0)));
      std::sort(result[table].begin(),result[table].end());
    }
    return result;
  };
  std::map<std::string,std::vector<std::string>> expected; std::string manifest;
  { auto c = value(cat::Catalog::open(source.root,options()));
    for (int revision = 2; revision <= 3; ++revision) {
      repo::ChangeRequest r; r.ref = source.asset.ref; r.expected_rev = static_cast<std::uint64_t>(revision-1);
      repo::SetMetadata m; m.field = "description"; m.value.emplace("Portable revision "+std::to_string(revision)); r.ops.emplace_back(m); value(c.change(r));
    }
    expected = comparable(c.library()); manifest = value(c.library().assets().snapshot(source.asset.ref)).manifest_text();
    cat::LibraryVerifyOptions o; o.level = cat::LibraryVerifyLevel::full; EXPECT_EQ(value(c.verify(o)).ok,1u);
  }
  const auto transport = fs::path(AM_LIBRARY_TEST_ROOT)/id()/"ready.zip"; fs::create_directories(transport.parent_path()); fs::copy_file(source.path(),transport);
  const auto digest = hash(transport); ASSERT_TRUE(fs::remove(source.path()));
  for (const auto& f : fs::directory_iterator(source.root)) if (f.is_regular_file() && f.path().extension() == ".bin") fs::remove(f.path());
  for (const bool import : {true,false}) {
    SCOPED_TRACE(import ? "import_ready" : "disk_rebuild"); const auto root = fresh(); auto c = value(cat::Catalog::open(root,options())); value(c.indexer().rebuild());
    if (import) { const auto imported = value(c.import_ready({transport,{}})); EXPECT_EQ(imported.status,"imported"); }
    else { const auto target = asset_path(root,source.asset.ref.asset_id); fs::create_directories(target.parent_path()); fs::copy_file(transport,target); EXPECT_EQ(value(c.indexer().rebuild()).indexed,1u); }
    EXPECT_EQ(hash(asset_path(root,source.asset.ref.asset_id)),digest);
    const repo::AssetRef ref{c.library().config().library_id,source.asset.ref.asset_id};
    const auto copied = value(c.library().assets().snapshot(ref)); EXPECT_EQ(copied.manifest_text(),manifest); EXPECT_EQ(copied.revision(),3u);
    EXPECT_EQ(comparable(c.library()),expected);
    EXPECT_EQ(Json::parse(copied.manifest_text())["history"],Json::parse(manifest)["history"]);
    cat::LibraryVerifyOptions o; o.level = cat::LibraryVerifyLevel::full; const auto checked = value(c.verify(o)); EXPECT_EQ(checked.ok,1u); EXPECT_EQ(checked.corrupt+checked.warn+checked.unsupported+checked.unreadable,0u);
  }
}
TEST(LibraryVerify, PurgeAllWithMissingTrashIsEmptySuccess) {
  RecordProperty("req","K-22"); World w; ASSERT_TRUE(fs::remove(w.root/"trash"));
#ifdef _WIN32
  const auto result = value(am::application::purge_library(w.root,{},true,true)); EXPECT_EQ(result.status,am::application::ExitCode::success); EXPECT_TRUE(result.value["results"].empty());
#else
  // The service selection is covered on fixed NTFS; Linux production writes
  // remain intentionally unavailable. Verification itself has no such skip.
  GTEST_SKIP() << "Production writes require a verified fixed NTFS volume; verification itself is tested without SKIP";
#endif
}
TEST(JobRecovery, CommitRecoveryPrecedesJobsAndReadOnlyPreservesEverything) {
  RecordProperty("req","NFR-14.4.3|NFR-14.4.9|K-123"); World w; crash(w,"change","after_verified_journal");
  const auto pending=w.pending(); ASSERT_EQ(pending.size(),1u);
  std::string retry, exhausted; std::map<std::string,am::contracts::JobRecord> unchanged;
  using State=am::contracts::JobState;
  {
    auto l=value(lib::Library::open(w.root,options())); am::db::StateJobStore store(l.database(lib::DatabaseKind::state));
    am::contracts::JobSpec spec; spec.type="verify";
    retry=value(store.enqueue(spec)).job_id; value(store.transition(retry,State::queued,State::running));
    exhausted=value(store.enqueue(spec)).job_id;
    for(int i=0;i<3;++i) {
      value(store.transition(exhausted,State::queued,State::running));
      if(i<2) { value(store.transition(exhausted,State::running,State::failed)); value(store.transition(exhausted,State::failed,State::queued)); }
    }
    for(auto state:{State::queued,State::paused,State::needs_decision,State::waiting_resources,State::resource_error,State::cannot_continue_safely,State::succeeded,State::failed,State::cancelled}) {
      const auto job=value(store.enqueue(spec)).job_id;
      if(state==State::queued) { unchanged[job]=*value(store.get(job)); continue; }
      value(store.transition(job,State::queued,State::running)); am::contracts::JobPatch patch;
      value(store.report(job,{{1},{2},"files","Inspecting"}));
      if(state==State::needs_decision) patch.decision_json="{\"kind\":\"test\"}";
      value(store.transition(job,State::running,state,patch));
      unchanged[job]=*value(store.get(job));
    }
  }
  const auto state_hash=hash(w.root/".am/state.sqlite"), zip_hash=hash(w.path());
  {
    auto c=value(cat::Catalog::open(w.root,options(lib::Mode::read_only)));
    EXPECT_TRUE(warning(c.open_report(),"am.jobs.recovery_pending")); EXPECT_TRUE(c.open_report().jobs_requeued.empty());
    EXPECT_TRUE(c.open_report().jobs_failed.empty());
  }
  EXPECT_EQ(hash(w.root/".am/state.sqlite"),state_hash); EXPECT_EQ(hash(w.path()),zip_hash);
  {
    auto o=options(); o.max_attempts=3; auto c=value(cat::Catalog::open(w.root,o));
    EXPECT_EQ(c.open_report().jobs_requeued,(std::vector<std::string>{retry})); EXPECT_EQ(c.open_report().jobs_failed,(std::vector<std::string>{exhausted}));
    am::db::StateJobStore store(c.library().database(lib::DatabaseKind::state));
    const auto first=*value(store.get(retry)), last=*value(store.get(exhausted));
    EXPECT_EQ(first.state,State::queued); EXPECT_EQ(first.attempt,1); EXPECT_EQ(last.state,State::failed); EXPECT_EQ(last.attempt,3);
    ASSERT_TRUE(last.error_json); EXPECT_EQ(Json::parse(*last.error_json)["message_id"],"am.jobs.attempts_exhausted"); EXPECT_EQ(Json::parse(*last.error_json)["code"],"am.incomplete"); EXPECT_TRUE(last.finished_at);
    for(const auto& [job,original]:unchanged) {
      const auto actual=*value(store.get(job)); EXPECT_EQ(actual.state,original.state); EXPECT_EQ(actual.attempt,original.attempt);
      EXPECT_EQ(actual.params_json,original.params_json); EXPECT_EQ(actual.decision_json,original.decision_json);
      EXPECT_EQ(actual.result_json,original.result_json); EXPECT_EQ(actual.error_json,original.error_json);
      EXPECT_EQ(actual.created_at,original.created_at); EXPECT_EQ(actual.started_at,original.started_at); EXPECT_EQ(actual.finished_at,original.finished_at);
      EXPECT_EQ(actual.position,original.position); EXPECT_EQ(actual.priority,original.priority);
      EXPECT_EQ(actual.progress.done,original.progress.done); EXPECT_EQ(actual.progress.total,original.progress.total);
      EXPECT_EQ(actual.progress.unit,original.progress.unit); EXPECT_EQ(actual.progress.stage,original.progress.stage);
    }
    am::db::OperationLog log(c.library().database(lib::DatabaseKind::state)); const auto rows=value(log.query()); ASSERT_EQ(rows.size(),3u);
    for(const auto& row:rows) EXPECT_EQ(row.code,"library.recovery");
    EXPECT_EQ(rows[0].asset_id,w.asset.ref.asset_id); EXPECT_EQ(rows[0].level,"info");
    EXPECT_EQ(Json::parse(*rows[0].detail_json)["op_id"],pending[0].op_id);
    EXPECT_EQ(Json::parse(*rows[0].detail_json)["outcome"],"completed");
    EXPECT_EQ(rows[1].job_id,retry); EXPECT_EQ(rows[2].job_id,exhausted);
    EXPECT_FALSE(rows[1].asset_id); EXPECT_FALSE(rows[2].asset_id);
    EXPECT_EQ(rows[1].level,"info"); EXPECT_EQ(rows[2].level,"error");
    EXPECT_EQ(Json::parse(*rows[1].detail_json)["job_id"],retry); EXPECT_EQ(Json::parse(*rows[1].detail_json)["outcome"],"queued");
    EXPECT_EQ(Json::parse(*rows[2].detail_json)["job_id"],exhausted); EXPECT_EQ(Json::parse(*rows[2].detail_json)["outcome"],"failed");
    EXPECT_EQ(value(c.indexer().list())[0].revision,2); w.clean();
  }
  auto c=value(cat::Catalog::open(w.root,options())); EXPECT_TRUE(c.open_report().jobs_requeued.empty()); EXPECT_TRUE(c.open_report().jobs_failed.empty());
}
TEST(JobRecovery, StoreFailureWarnsAfterZipRecoveryWithoutPreventingOpen) {
  RecordProperty("req","NFR-14.4.3|K-123"); World w; crash(w,"change","after_verified_journal");
  { auto l=value(lib::Library::open(w.root,options())); auto db=am::db::Connection::borrow(l.database(lib::DatabaseKind::state)); value(db.exec("ALTER TABLE jobs RENAME COLUMN state TO broken_state")); }
  auto c=value(cat::Catalog::open(w.root,options())); EXPECT_TRUE(warning(c.open_report(),"am.jobs.recovery_failed"));
  EXPECT_EQ(value(c.indexer().list())[0].revision,2); w.clean(); EXPECT_FALSE(value(c.indexer().rebuild_required()));
}
#endif
