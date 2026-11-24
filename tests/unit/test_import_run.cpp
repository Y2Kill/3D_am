#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <set>

#include "am/application/import_service.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/import/run.hpp"

namespace {
namespace fs = std::filesystem;
namespace imp = am::import;
namespace jc = am::contracts;
using J = imp::PlanJson;
template <class T>
T take(am::Result<T> r) {
  if (!r) throw std::runtime_error(r.error().message_id + ": " + am::to_log_string(r.error()));
  return std::move(*r);
}
void take(am::Result<void> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
}
void write(const fs::path& path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream f(am::paths::native(path), std::ios::binary);
  f << bytes;
  if (!f) throw std::runtime_error("Fixture write failed");
}
std::string bytes(const fs::path& path) {
  std::ifstream f(am::paths::native(path), std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
class ImportRun : public testing::Test {
 protected:
  fs::path work, source, library, plan_file;
  imp::ImportPlan plan;
  std::optional<am::catalog::Catalog> catalog;
  void SetUp() override {
#ifndef _WIN32
    GTEST_SKIP() << "Import run writes require verified fixed NTFS; WSL/DrvFS remains read-only";
#endif
    work = fs::path(AM_IMPORT_SCAN_ROOT) / "run" / am::Uuid::v4().to_string();
    source = work / "source";
    library = work / "library";
    plan_file = work / "plan.json";
    fs::create_directories(source);
    take(am::library::Library::init(library));
    catalog.emplace(take(am::catalog::Catalog::open(library)));
  }
  void scan(std::vector<fs::path> roots = {}, bool single = false, bool hash = false,
            std::string_view profile = "default") {
    imp::ScanRequest r;
    r.roots = roots.empty() ? std::vector<fs::path>{source} : roots;
    r.single_candidate = single;
    r.hash_inputs = hash;
    r.profile = take(am::application::load_import_profile(profile));
    plan = take(imp::scan(r));
  }
  void save() { write(plan_file, imp::serialize_plan(plan)); }
  imp::QueuedRun enqueue() {
    save();
    return take(imp::enqueue_run(*catalog, plan_file));
  }
  am::jobs::RunSummary execute(imp::RunHooks hooks = {}, unsigned parallel = 2) {
    am::jobs::HandlerRegistry registry;
    take(imp::register_import_jobs(registry, *catalog, "test", std::move(hooks)));
    am::jobs::ExecutorOptions options;
    options.limits = {parallel, parallel, 1, parallel, parallel, parallel, parallel, parallel};
    options.auto_retries = 0;
    am::jobs::ResourceManager resources(options.limits);
    am::jobs::Executor executor(catalog->job_store(), resources, registry, options);
    return take(executor.run_until_idle());
  }
  std::vector<jc::JobRecord> children(const imp::QueuedRun& q) {
    jc::JobFilter f;
    f.parent_id = q.job_id;
    return take(catalog->job_store().list(f));
  }
  J result(const imp::QueuedRun& q) { return take(imp::run_summary(*catalog, q)); }
  am::repository::AssetRef ref(const J& row) {
    return {catalog->library().config().library_id, row.at("asset_id").get<std::string>()};
  }
  J manifest(const J& row) {
    auto snap = take(catalog->library().assets().snapshot(ref(row)));
    return J::parse(snap.manifest().known().dump());
  }
  void clean() {
    const auto tmp = library / ".am/.am-tmp";
    EXPECT_TRUE(!fs::exists(tmp) || fs::is_empty(tmp));
    const auto report = take(catalog->verify({am::catalog::LibraryVerifyLevel::full}));
    EXPECT_EQ(report.corrupt, 0u);
    EXPECT_EQ(report.unreadable, 0u);
    EXPECT_FALSE(report.database_failed);
  }
};
}  // namespace
TEST_F(ImportRun, DirectoryAccountsForEveryNodeAndPreservesBytes) {
  RecordProperty("req", "SC-1|K-5|ADR-0025");
  write(source / "nested" / "part.bin", "payload");
  write(source / ".DS_Store", "metadata");
  write(source / "link", "not opened");
  scan();
  auto& c = plan.value["candidates"][0];
  c["action"] = "import";
  for (auto& n : plan.value["nodes"])
    for (auto& e : c["entries"])
      if (e["node"] == n["id"]) {
        if (n["name"] == ".DS_Store") {
          e["decision"] = "exclude";
          e["rule_id"] = "macos.ds_store";
          e["confidence"] = "certain";
        }
        if (n["name"] == "link") {
          n["kind"] = "symlink";
          e["decision"] = "skip";
        }
      }
  const auto q = enqueue();
  const auto original = bytes(plan_file);
  std::set<fs::path> opened;
  imp::RunHooks hooks;
  hooks.on_open = [&](const auto& file) { opened.insert(file); };
  execute(hooks);
  const auto row = result(q)["candidates"][0];
  ASSERT_EQ(row["status"], "created") << row.dump();
  const auto m = manifest(row);
  const auto& input = m["inputs"][0];
  ASSERT_EQ(input["objects"].size(), 1u);
  EXPECT_EQ(input["entries"].size(), plan.value["nodes"].size());
  EXPECT_EQ(input["completeness"], "complete");
  unsigned excluded = 0, links = 0, dirs = 0;
  for (const auto& e : input["entries"]) {
    if (e["result"] == "excluded") {
      ++excluded;
      EXPECT_EQ(e["exclusion"]["by"], "auto");
    }
    if (e["result"] == "not_processed") {
      ++links;
      EXPECT_EQ(e["problem"]["code"], "link_not_followed");
    }
    if (e["entry_kind"] == "directory") ++dirs;
  }
  EXPECT_EQ(excluded, 1u);
  EXPECT_EQ(links, 1u);
  EXPECT_EQ(dirs, 2u);
  EXPECT_FALSE(opened.contains(source / ".DS_Store"));
  EXPECT_FALSE(opened.contains(source / "link"));
  auto snapshot = take(catalog->library().assets().snapshot(ref(row)));
  am::asset_zip::ReadBudget budget(1000000);
  for (const auto& e : snapshot.archive().entries())
    if (e.raw_name != "manifest.json") {
      std::string payload;
      auto stream = take(snapshot.archive().open_entry(e.index, budget, true));
      std::array<std::byte, 1024> buffer{};
      for (;;) {
        const auto n = take(stream.read(buffer));
        if (!n) break;
        payload.append(reinterpret_cast<const char*>(buffer.data()), n);
      }
      EXPECT_EQ(payload, "payload");
      EXPECT_TRUE(e.raw_name.ends_with("/nested/part.bin"));
    }
  EXPECT_EQ(bytes(plan_file), original);
  clean();
}
TEST_F(ImportRun, UserExclusionAndOverrideOfRuleAreExplicit) {
  RecordProperty("req", "SC-1|K-5");
  write(source / "user.bin", "excluded");
  write(source / "desktop.ini", "included by user");
  scan();
  auto& c = plan.value["candidates"][0];
  c["action"] = "import";
  for (auto& e : c["entries"])
    for (const auto& n : plan.value["nodes"])
      if (e["node"] == n["id"]) e["decision"] = n["name"] == "user.bin" ? "exclude" : "include";
  const auto q = enqueue();
  execute();
  auto m = manifest(result(q)["candidates"][0]);
  unsigned excluded = 0, included = 0;
  for (const auto& e : m["inputs"][0]["entries"]) {
    if (e["result"] == "excluded") {
      ++excluded;
      EXPECT_EQ(e["exclusion"]["by"], "user");
    }
    if (e.contains("file_id")) ++included;
  }
  EXPECT_EQ(excluded, 1u);
  EXPECT_EQ(included, 1u);
  clean();
}
TEST_F(ImportRun, UnavailableAskStrictPartialAndExcludedHaveDistinctOutcomes) {
  RecordProperty("req", "SC-1|K-5|NFR-14.4.3");
  write(source / "part", "available");
  write(source / "cloud", "placeholder");
  scan();
  auto& c = plan.value["candidates"][0];
  std::string cloud;
  for (auto& n : plan.value["nodes"])
    if (n["name"] == "cloud") {
      cloud = n["id"];
      n["status"] = "unavailable";
      n["problem"] = {{"code", "cloud_placeholder"}, {"message_id", "am.import.cloud_placeholder"}};
    }
  for (auto& e : c["entries"])
    if (e["node"] == cloud) e["decision"] = "ask";
  c["action"] = "import";
  auto q = enqueue();
  EXPECT_TRUE(children(q).empty());
  EXPECT_EQ(result(q)["candidates"][0]["message_id"], "am.import.run.unresolved");
  EXPECT_TRUE(result(q)["candidates"][0]["has_unavailable"]);
  for (auto& e : c["entries"])
    if (e["node"] == cloud) e["decision"] = "include";
  q = enqueue();
  execute();
  EXPECT_EQ(result(q)["candidates"][0]["status"], "failed");
  EXPECT_EQ(result(q)["candidates"][0]["error"]["message_id"], "am.import.run.source_unavailable");
  plan.value["mode"] = "partial";
  q = enqueue();
  execute();
  auto m = manifest(result(q)["candidates"][0]);
  EXPECT_EQ(m["inputs"][0]["completeness"], "incomplete");
  bool unavailable = false;
  for (const auto& e : m["inputs"][0]["entries"])
    if (e["result"] == "unavailable") {
      unavailable = true;
      EXPECT_EQ(e["problem"]["code"], "cloud_placeholder");
    }
  EXPECT_TRUE(unavailable);
  plan.value["mode"] = "strict";
  for (auto& e : c["entries"])
    if (e["node"] == cloud) e["decision"] = "exclude";
  q = enqueue();
  imp::RunHooks hooks;
  hooks.on_open = [&](const auto& f) { EXPECT_NE(f, source / "cloud"); };
  execute(hooks);
  m = manifest(result(q)["candidates"][0]);
  EXPECT_EQ(m["inputs"][0]["completeness"], "complete");
  clean();
}
TEST_F(ImportRun, SourceChangeBeforeAndAfterCommitNeverSilentlySucceeds) {
  RecordProperty("req", "SC-1|NFR-14.4.3");
  write(source / "part", "before");
  scan();
  const auto q = enqueue();
  write(source / "part", "changed size");
  execute();
  EXPECT_EQ(result(q)["candidates"][0]["error"]["message_id"], "am.import.source_changed");
  scan();
  const auto next = enqueue();
  imp::RunHooks hooks;
  hooks.after_created = [&](auto) -> am::Result<void> {
    write(source / "part", "changed during import");
    return {};
  };
  execute(hooks);
  const auto row = result(next)["candidates"][0];
  EXPECT_EQ(row["error"]["message_id"], "am.import.source_changed_during_import");
  EXPECT_TRUE(row["in_trash"]);
  EXPECT_TRUE(fs::exists(library / "trash" / (row["asset_id"].get<std::string>() + ".zip")));
  const auto parent = take(catalog->job_store().get(next.job_id));
  ASSERT_TRUE(parent->result_json);
  EXPECT_NE(parent->result_json->find("source_changed_during_import"), std::string::npos);
  clean();
}
TEST_F(ImportRun, PlanShaProtectsQueuedWorkAndRepeatUsesPersistedIds) {
  RecordProperty("req", "SC-1|K-121|NFR-14.4.3");
  write(source / "part", "payload");
  scan();
  const auto q = enqueue();
  execute();
  const auto row = result(q)["candidates"][0];
  const auto original = manifest(row);
  const auto again = take(imp::enqueue_run(*catalog, plan_file));
  EXPECT_TRUE(again.reused);
  EXPECT_EQ(result(again)["candidates"][0]["status"], "already_imported");
  EXPECT_EQ(take(catalog->indexer().count()), 1);
  EXPECT_EQ(manifest(row), original);
  scan();
  plan.value["candidates"][0]["title_hint"] = "Changed plan";
  const auto changed = enqueue();
  write(plan_file, bytes(plan_file) + " ");
  execute();
  const auto child = children(changed).at(0);
  EXPECT_EQ(child.state, jc::JobState::failed);
  ASSERT_TRUE(child.error_json);
  EXPECT_NE(child.error_json->find("plan_changed"), std::string::npos);
  clean();
}
TEST_F(ImportRun, RecoveryAfterCommitFindsInputAndDoesNotRewriteAsset) {
  RecordProperty("req", "SC-1|K-121|NFR-14.4.3");
  write(source / "part", "payload");
  scan();
  const auto q = enqueue();
  imp::RunHooks hooks;
  hooks.after_created = [](auto) -> am::Result<void> {
    return am::Error::make(am::Errc::io_error, "Injected post-commit failure").id("test.after_commit");
  };
  execute(hooks);
  auto row = result(q)["candidates"][0];
  ASSERT_EQ(row["status"], "failed");
  const auto before = manifest(row);
  const auto child = children(q)[0];
  const auto child_params = J::parse(child.params_json);
  take(catalog->job_store().transition(q.job_id, jc::JobState::failed, jc::JobState::queued));
  take(catalog->job_store().transition(child.job_id, jc::JobState::failed, jc::JobState::queued));
  execute();
  row = result(q)["candidates"][0];
  EXPECT_EQ(row["status"], "created");
  EXPECT_EQ(manifest(row), before);
  EXPECT_EQ(take(catalog->indexer().count()), 1);
  EXPECT_EQ(J::parse(children(q)[0].params_json), child_params);
  clean();
}
TEST_F(ImportRun, ForeignAssetAtAllocatedIdIsNeverChanged) {
  RecordProperty("req", "SC-1|NFR-14.4.3");
  write(source / "part", "payload");
  scan();
  const auto q = enqueue();
  const auto params = J::parse(children(q)[0].params_json);
  am::repository::CreateRequest request;
  request.asset_id = params["asset_id"].get<std::string>();
  take(catalog->create(request));
  execute();
  const auto row = result(q)["candidates"][0];
  EXPECT_EQ(row["error"]["message_id"], "am.import.run.asset_id_conflict");
  EXPECT_EQ(take(catalog->indexer().count()), 1);
  clean();
}
TEST_F(ImportRun, PartialReadFailureRollsBackAndRetriesOnlyTheFailedSource) {
  RecordProperty("req", "SC-1|K-5|NFR-14.4.3");
  write(source / "good", "good");
  write(source / "bad", "bad");
  scan();
  plan.value["mode"] = "partial";
  const auto q = enqueue();
  unsigned attempts = 0;
  imp::RunHooks hooks;
  hooks.before_attempt = [&](auto&) {
    if (++attempts == 1) fs::remove(source / "bad");
  };
  execute(hooks);
  const auto row = result(q)["candidates"][0];
  ASSERT_EQ(row["status"], "created") << row.dump();
  const auto m = manifest(row);
  EXPECT_EQ(attempts, 2u);
  EXPECT_EQ(m["files"].size(), 1u);
  EXPECT_EQ(m["inputs"][0]["completeness"], "incomplete");
  unsigned failed = 0;
  for (const auto& e : m["inputs"][0]["entries"])
    if (e["result"] == "failed") ++failed;
  EXPECT_EQ(failed, 1u);
  clean();
}
TEST_F(ImportRun, SingleKeepsRealRootsSeparateAndRenamesCollidingRootNames) {
  RecordProperty("req", "SC-2|K-5|ADR-0012");
  const auto first = source / "one" / "same", second = source / "two" / "same", file = source / "file.bin";
  write(first / "part", "one");
  write(second / "part", "two");
  write(file, "three");
  scan({first, second, file}, true);
  const auto q = enqueue();
  execute();
  const auto row = result(q)["candidates"][0];
  ASSERT_EQ(row["status"], "created") << row.dump();
  const auto m = manifest(row);
  const auto& input = m["inputs"][0];
  ASSERT_EQ(input["objects"].size(), 3u);
  EXPECT_EQ(m["files"].size(), 3u);
  std::set<std::string> objects;
  for (const auto& o : input["objects"]) objects.insert(o["id"]);
  unsigned renamed = 0;
  for (const auto& e : input["entries"]) {
    EXPECT_TRUE(objects.contains(e["object_id"]));
    if (e["result"] == "included_renamed") {
      ++renamed;
      EXPECT_EQ(e["rename_reason"], "collision");
    }
  }
  EXPECT_EQ(renamed, 1u);
  clean();
}
TEST_F(ImportRun, DirectoryProvenanceClaimsAreUnconfirmed) {
  RecordProperty("req", "SC-1|K-5");
  write(source / "Channel" / "Model" / "part", "payload");
  scan({}, false, false, "telegram");
  const auto q = enqueue();
  execute();
  const auto m = manifest(result(q)["candidates"][0]);
  ASSERT_EQ(m["origin"]["sources"].size(), 1u);
  const auto& origin_source = m["origin"]["sources"][0];
  EXPECT_EQ(origin_source["kind"], "telegram_channel");
  EXPECT_EQ(origin_source["claims"][0]["method"], "directory_inference");
  EXPECT_FALSE(origin_source["claims"][0].contains("review"));
  EXPECT_EQ(m["inputs"][0]["source_refs"][0], origin_source["id"]);
  clean();
}
TEST_F(ImportRun, ParallelCandidatesHaveConsistentIndexAndFullVerification) {
  RecordProperty("req", "SC-1|K-115|NFR-14.4.3");
  std::vector<fs::path> roots;
  for (unsigned i = 0; i < 4; ++i) {
    const auto root = source / std::to_string(i);
    write(root / "part", std::string(65537, 'a' + static_cast<char>(i)));
    roots.push_back(root);
  }
  scan(roots);
  const auto q = enqueue();
  execute({}, 4);
  const auto rows = result(q)["candidates"];
  ASSERT_EQ(rows.size(), 4u);
  for (const auto& row : rows) EXPECT_EQ(row["status"], "created") << row.dump();
  EXPECT_EQ(take(catalog->indexer().count()), 4);
  clean();
}

TEST_F(ImportRun, ReadyAssetsAreClassifiedWithoutReplacingExistingIds) {
  RecordProperty("req", "SC-3|K-5|ADR-0025");
  const auto zip = source / "ready.zip";
  fs::copy_file(fs::path(AM_GOLDEN_FIXTURES) / "pos-minimal" / "asset.zip", zip);
  scan();
  const auto q = enqueue();
  execute();
  const auto row = result(q)["candidates"][0];
  ASSERT_EQ(row["status"], "imported") << row.dump();
  EXPECT_EQ(row["classification"], "new");
  const auto before = manifest(row);
  plan.value["candidates"][0]["title_hint"] = "Fresh plan";
  const auto next = enqueue();
  execute();
  const auto conflict = result(next)["candidates"][0];
  EXPECT_EQ(conflict["status"], "deferred");
  EXPECT_EQ(conflict["classification"], "identical");
  EXPECT_EQ(children(next)[0].state, jc::JobState::needs_decision);
  ASSERT_TRUE(children(next)[0].result_json);
  EXPECT_EQ(manifest(row), before);
  EXPECT_EQ(take(catalog->indexer().count()), 1);
  const auto decision_child = children(next)[0];
  auto decision = J::parse(*decision_child.decision_json);
  decision["answer"] = {{"option", "skip"}};
  jc::JobPatch answer;
  answer.decision_json = decision.dump();
  take(catalog->job_store().transition(decision_child.job_id, jc::JobState::needs_decision, jc::JobState::queued,
                                       answer));
  fs::remove(zip);
  imp::RunHooks hooks;
  hooks.on_open = [&](const auto& file) { EXPECT_NE(file, zip); };
  execute(hooks);
  EXPECT_EQ(result(next)["candidates"][0]["status"], "skipped");
  const auto repeated = take(imp::enqueue_run(*catalog, plan_file));
  EXPECT_EQ(result(repeated)["candidates"][0]["status"], "skipped");
  clean();
}
TEST_F(ImportRun, SourceThatBecameCloudPlaceholderIsNotOpened) {
  RecordProperty("req", "SC-1|K-5|NFR-14.4.3");
  write(source / "cloud", "placeholder");
  scan();
  const auto q = enqueue();
  imp::RunHooks hooks;
  hooks.node_data = [&](const auto& file) -> am::Result<am::paths::WindowsNodeData> {
    auto data = am::paths::windows_node_data(file);
    if (data && file == source / "cloud") data->attributes |= 0x1000u;
    return data;
  };
  hooks.on_open = [&](const auto& file) { EXPECT_NE(file, source / "cloud"); };
  execute(hooks);
  EXPECT_EQ(result(q)["candidates"][0]["error"]["message_id"], "am.import.run.source_unavailable");
  EXPECT_EQ(take(catalog->indexer().count()), 0);
  clean();
}
TEST_F(ImportRun, InputObjectFilesWithoutExplicitEntriesStillCheckFreshnessAndAvailability) {
  RecordProperty("req", "SC-1|K-5|NFR-14.4.3");
  write(source / "part", "before");
  scan();
  plan.value["candidates"][0]["entries"] = J::array();
  const auto changed = enqueue();
  write(source / "part", "changed after scan");
  execute();
  EXPECT_EQ(result(changed)["candidates"][0]["error"]["message_id"], "am.import.source_changed");
  for (auto& n : plan.value["nodes"])
    if (n["name"] == "part") {
      n["status"] = "unavailable";
      n["problem"] = {{"code", "cloud_placeholder"}, {"message_id", "am.import.cloud_placeholder"}};
    }
  const auto unavailable = enqueue();
  imp::RunHooks hooks;
  hooks.on_open = [&](const auto& file) { EXPECT_NE(file, source / "part"); };
  execute(hooks);
  EXPECT_EQ(result(unavailable)["candidates"][0]["error"]["message_id"], "am.import.run.source_unavailable");
  EXPECT_EQ(take(catalog->indexer().count()), 0);
  clean();
}
TEST_F(ImportRun, UnsupportedArchivesAndUnresolvedOrSkippedCandidatesDoNotCreateChildren) {
  RecordProperty("req", "SC-2|SC-3|K-5");
  write(source / "part", "payload");
  scan();
  auto& c = plan.value["candidates"][0];
  c["inputs"][0]["kind"] = "archive";
  const auto archive = enqueue();
  EXPECT_TRUE(children(archive).empty());
  EXPECT_EQ(result(archive)["candidates"][0]["message_id"], "am.import.run.archive_input_unsupported");
  c["inputs"][0]["kind"] = "directory";
  c["action"] = "ask";
  const auto unresolved = enqueue();
  EXPECT_TRUE(children(unresolved).empty());
  EXPECT_EQ(result(unresolved)["candidates"][0]["message_id"], "am.import.run.unresolved");
  c["action"] = "skip";
  for (const auto& object : c["inputs"][0]["objects"])
    if (object.contains("node"))
      c["matches"].push_back(
          {{"kind", "sha256_input"}, {"node", object["node"]}, {"asset_id", am::Uuid::v4().to_string()}});
  const auto skipped = enqueue();
  EXPECT_TRUE(children(skipped).empty());
  EXPECT_EQ(result(skipped)["candidates"][0]["status"], "skipped");
  EXPECT_EQ(take(catalog->indexer().count()), 0);
  clean();
}
TEST_F(ImportRun, ExpectedPlanShaDetectsSameSizeAndMtimeContentChanges) {
  RecordProperty("req", "SC-1|ADR-0038|NFR-14.4.3");
  write(source / "part", "before");
  scan({}, false, true);
  const auto timestamp = fs::last_write_time(source / "part");
  const auto q = enqueue();
  write(source / "part", "after!");
  fs::last_write_time(source / "part", timestamp);
  execute();
  const auto row = result(q)["candidates"][0];
  EXPECT_EQ(row["error"]["message_id"], "am.import.source_changed");
  EXPECT_EQ(take(catalog->indexer().count()), 0);
  clean();
}
TEST_F(ImportRun, StrictReadFailureDoesNotRetry) {
  RecordProperty("req", "SC-1|K-5|NFR-14.4.3");
  write(source / "part", "payload");
  scan();
  const auto q = enqueue();
  unsigned attempts = 0;
  imp::RunHooks hooks;
  hooks.before_attempt = [&](auto&) {
    ++attempts;
    fs::remove(source / "part");
  };
  execute(hooks);
  EXPECT_EQ(result(q)["candidates"][0]["status"], "failed");
  EXPECT_EQ(attempts, 1u);
  EXPECT_EQ(take(catalog->indexer().count()), 0);
  clean();
}
TEST_F(ImportRun, CancellationAndPauseRespectSafePointsAndKeepCompletedCandidates) {
  RecordProperty("req", "K-121|NFR-14.4.3");
  write(source / "part", "payload");
  scan();
  const auto q = enqueue();
  const auto child = children(q)[0];
  am::jobs::HandlerRegistry registry;
  am::jobs::Executor* running = nullptr;
  bool pause = true;
  imp::RunHooks hooks;
  hooks.before_attempt = [&](auto&) {
    if (pause) {
      pause = false;
      take(running->pause(child.job_id));
    }
  };
  take(imp::register_import_jobs(registry, *catalog, "test", hooks));
  am::jobs::ExecutorOptions options;
  options.auto_retries = 0;
  am::jobs::ResourceManager resources(options.limits);
  am::jobs::Executor executor(catalog->job_store(), resources, registry, options);
  running = &executor;
  take(executor.run_until_idle());
  EXPECT_EQ(children(q)[0].state, jc::JobState::paused);
  EXPECT_EQ(take(catalog->indexer().count()), 0);
  take(executor.resume(child.job_id));
  take(executor.run_until_idle());
  EXPECT_EQ(children(q)[0].state, jc::JobState::succeeded);
  clean();
  scan();
  plan.value["candidates"][0]["title_hint"] = "Cancel attempt";
  const auto next = enqueue();
  const auto next_child = children(next)[0];
  am::jobs::HandlerRegistry cancel_registry;
  am::jobs::Executor* cancelling = nullptr;
  imp::RunHooks cancel_hooks;
  cancel_hooks.before_attempt = [&](auto&) { take(cancelling->cancel(next_child.job_id)); };
  take(imp::register_import_jobs(cancel_registry, *catalog, "test", cancel_hooks));
  am::jobs::Executor cancel_executor(catalog->job_store(), resources, cancel_registry, options);
  cancelling = &cancel_executor;
  take(cancel_executor.run_until_idle());
  EXPECT_EQ(children(next)[0].state, jc::JobState::cancelled);
  jc::JobFilter filter;
  filter.states = {jc::JobState::running};
  EXPECT_TRUE(take(catalog->job_store().list(filter)).empty());
  EXPECT_EQ(take(catalog->indexer().count()), 1);
  clean();
}
TEST_F(ImportRun, InvalidUtf16NamesRemainRawFactsInObjectAndEntry) {
  RecordProperty("req", "SC-1|K-5|ADR-0012");
#ifdef _WIN32
  std::wstring name = L"bad";
  name += static_cast<wchar_t>(0xd800);
  name += L".bin";
  const auto file = source / fs::path(name);
  write(file, "payload");
  scan({file});
  const auto q = enqueue();
  execute();
  const auto row = result(q)["candidates"][0];
  ASSERT_EQ(row["status"], "created") << row.dump();
  const auto m = manifest(row);
  const auto& input = m["inputs"][0];
  ASSERT_EQ(input["objects"].size(), 1u);
  EXPECT_EQ(input["objects"][0]["name_raw"]["base64"], "YgBhAGQAANguAGIAaQBuAA==");
  EXPECT_EQ(input["objects"][0]["name_raw"]["encoding_guess"], "utf-16le");
  EXPECT_EQ(input["entries"][0]["path_raw"], input["objects"][0]["name_raw"]);
  EXPECT_EQ(input["entries"][0]["rename_reason"], "encoding");
  clean();
#endif
}
