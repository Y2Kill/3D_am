#include <gtest/gtest.h>

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <thread>

#include "am/application/job_service.hpp"
#include "am/foundation/paths.hpp"
#include "am/foundation/uuid.hpp"
#include "am/services/catalog/verify_job.hpp"

namespace {
namespace fs = std::filesystem;
namespace app = am::application;
namespace j = am::jobs;
namespace c = am::contracts;
namespace cat = am::catalog;
namespace repo = am::repository;
template <class T>
T value(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(*r);
}
void value(am::Result<void> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
}
fs::path fresh() {
  const auto root = fs::path(AM_LIBRARY_TEST_ROOT) / am::Uuid::v4().to_string();
  value(am::library::Library::init(root));
  return root;
}
am::library::OpenOptions options() {
  am::library::OpenOptions o;
  o.capabilities.emplace();
  o.capabilities->mode = repo::StorageMode::full;
  return o;
}
void write(const fs::path& path, std::string_view data) {
  fs::create_directories(path.parent_path());
  std::ofstream f(am::paths::native(path), std::ios::binary);
  f << data;
  if (!f) throw std::runtime_error("Fixture write failed");
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool ready = false;
  void signal() {
    const std::lock_guard lock(mutex);
    ready = true;
    changed.notify_all();
  }
  bool wait() {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(15), [&] { return ready; });
  }
};
std::string create(cat::Catalog& catalog) {
  repo::CreateRequest request;
  request.metadata = repo::Json{{"title", "Verification fixture"}};
  return value(catalog.create(request)).change.ref.asset_id;
}
class StopWrapper final : public j::JobHandler {
 public:
  StopWrapper(std::unique_ptr<j::JobHandler> handler, Gate& stopped)
      : handler_(std::move(handler)), stopped_(stopped) {}
  j::JobOutcome run(j::JobContext& context) override {
    std::stop_callback callback(context.stop(), [&] { stopped_.signal(); });
    return handler_->run(context);
  }

 private:
  std::unique_ptr<j::JobHandler> handler_;
  Gate& stopped_;
};
struct ProgressBlock final : j::JobObserver {
  Gate entered, release;
  void on_event(const j::JobEvent& event) noexcept override {
    if (event.kind == j::EventKind::progress && event.job.progress.done == 0) {
      entered.signal();
      (void)release.wait();
    }
  }
};
}  // namespace
TEST(StorageMedia, FakeSysfsSupportsDiskPartitionRemovableAndMissingData) {
  RecordProperty("req", "K-115");
  const auto root = fs::path(AM_LIBRARY_TEST_ROOT) / am::Uuid::v4().to_string();
  const auto disk = root / "devices/disk";
  const auto partition = disk / "part1";
  fs::create_directories(partition);
  write(partition / "partition", "1");
  write(disk / "queue/rotational", "0");
  write(disk / "removable", "0");
  EXPECT_EQ(repo::detail::media_from_sysfs(root, partition), repo::MediaKind::ssd);
  write(disk / "queue/rotational", "1");
  EXPECT_EQ(repo::detail::media_from_sysfs(root, partition), repo::MediaKind::hdd);
  write(disk / "removable", "1");
  EXPECT_EQ(repo::detail::media_from_sysfs(root, partition), repo::MediaKind::removable);
  write(partition / "queue/rotational", "0");
  EXPECT_EQ(repo::detail::media_from_sysfs(root, partition), repo::MediaKind::removable);
  fs::remove(partition / "queue/rotational");
  write(disk / "removable", "0");
  write(disk / "queue/rotational", "invalid");
  EXPECT_EQ(repo::detail::media_from_sysfs(root, partition), repo::MediaKind::unknown);
  EXPECT_EQ(repo::detail::media_from_sysfs(root, root / "absent"), repo::MediaKind::unknown);
  EXPECT_EQ(repo::detail::media_from_sysfs(partition, disk), repo::MediaKind::unknown);
}
TEST(StorageMedia, NativeDetectionOnBuildFixtureHasADeviceIdentity) {
  RecordProperty("req", "K-115");
  const auto root = fresh();
  const auto result = repo::detect_media(root);
  EXPECT_FALSE(result.device_id.empty());
  EXPECT_GE(static_cast<int>(result.kind), 0);
  EXPECT_LE(static_cast<int>(result.kind), 4);
}
TEST(VerifyJob, CorruptZipSucceedsWithCountersAndCatalogLog) {
  RecordProperty("req", "K-121|NFR-14.4.3");
  const auto root = fresh();
  auto service = value(app::JobService::open(root, {}, options()));
  const auto asset = create(service.catalog());
  write(root / "assets" / asset.substr(34) / (asset + ".zip"), "broken ZIP");
  c::JobSpec spec;
  spec.type = "library.verify";
  spec.params_json = "{\"level\":\"quick\",\"include_trash\":true}";
  const auto job = value(service.enqueue(spec));
  const auto result = value(service.run());
  EXPECT_EQ(result.succeeded, 1u);
  const auto record = *value(service.store().get(job.job_id));
  EXPECT_EQ(record.state, c::JobState::succeeded);
  ASSERT_TRUE(record.result_json);
  EXPECT_EQ(app::Json::parse(*record.result_json)["corrupt"], 1);
  am::db::LogQuery query;
  query.limit = 100;
  const auto entries = value(service.catalog().operation_log().query(query));
  for (const auto* code : {"job.started", "library.verify", "job.finished"})
    EXPECT_TRUE(std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return entry.code == code; }));
}
TEST(VerifyJob, CancelAndShutdownAtBoundaryPreserveRequiredAttemptSemantics) {
  RecordProperty("req", "NFR-14.4.3|K-121");
  for (const bool shutdown : {false, true}) {
    const auto root = fresh();
    auto catalog = value(cat::Catalog::open(root, options()));
    create(catalog);
    j::HandlerRegistry original;
    value(cat::register_verify_job(original, catalog, "test-device"));
    const auto factory = *original.find("library.verify");
    Gate stopped;
    j::HandlerRegistry registry;
    value(registry.add("library.verify",
                       [&](const auto& record) { return std::make_unique<StopWrapper>(factory(record), stopped); }));
    c::JobSpec spec;
    spec.type = "library.verify";
    const auto job = value(catalog.job_store().enqueue(spec));
    j::ExecutorOptions config;
    config.limits.jobs = 1;
    j::ResourceManager resources(config.limits);
    ProgressBlock observer;
    j::Executor executor(catalog.job_store(), resources, registry, config, &observer);
    std::stop_source stop;
    std::optional<am::Result<j::RunSummary>> result;
    std::jthread run([&] { result.emplace(executor.run_until_idle(stop.get_token())); });
    ASSERT_TRUE(observer.entered.wait());
    if (shutdown)
      stop.request_stop();
    else
      value(executor.cancel(job.job_id));
    ASSERT_TRUE(stopped.wait());
    observer.release.signal();
    run.join();
    ASSERT_TRUE(result);
    ASSERT_TRUE(*result);
    const auto record = *value(catalog.job_store().get(job.job_id));
    EXPECT_EQ(record.state, shutdown ? c::JobState::queued : c::JobState::cancelled);
    EXPECT_EQ(record.attempt, shutdown ? 0 : 1);
  }
}
TEST(VerifyJob, InvalidParametersFailWithoutStoppingValidNeighbour) {
  RecordProperty("req", "K-121");
  const auto root = fresh();
  auto service = value(app::JobService::open(root, {}, options()));
  std::vector<std::string> ids;
  for (const auto* params : {"[]", "{\"level\":\"invalid\"}", "{\"include_trash\":0}", "{\"unknown\":true}"}) {
    c::JobSpec spec;
    spec.type = "library.verify";
    spec.params_json = params;
    ids.push_back(value(service.enqueue(spec)).job_id);
  }
  c::JobSpec good;
  good.type = "library.verify";
  const auto healthy = value(service.enqueue(good));
  const auto result = value(service.run());
  EXPECT_EQ(result.failed, 4u);
  EXPECT_EQ(result.succeeded, 1u);
  for (const auto& id : ids) {
    const auto record = *value(service.store().get(id));
    EXPECT_EQ(record.state, c::JobState::failed);
    EXPECT_NE(record.error_json->find("am.jobs.params_invalid"), std::string::npos);
  }
  EXPECT_EQ(value(service.store().get(healthy.job_id))->state, c::JobState::succeeded);
}
TEST(VerifyJob, JobLogFailureWarnsWithoutChangingSuccess) {
  RecordProperty("req", "K-121");
  const auto root = fresh();
  auto service = value(app::JobService::open(root, {}, options()));
  auto connection = am::db::Connection::borrow(service.catalog().library().database(am::library::DatabaseKind::state));
  value(
      connection.exec("CREATE TRIGGER reject_job_log BEFORE INSERT ON op_log WHEN NEW.code LIKE 'job.%' BEGIN SELECT "
                      "RAISE(FAIL,'injected'); END"));
  c::JobSpec spec;
  spec.type = "library.verify";
  const auto job = value(service.enqueue(spec));
  const auto result = value(service.run());
  EXPECT_EQ(result.succeeded, 1u);
  EXPECT_EQ(value(service.store().get(job.job_id))->state, c::JobState::succeeded);
  const auto warnings = service.diagnostics();
  EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(),
                          [](const auto& e) { return e.message_id == "am.catalog.log_failed"; }));
}
TEST(StateJobs, CooperativeRequeuePatchIsRestrictedAndFlooredAtZero) {
  RecordProperty("req", "NFR-14.4.3");
  const auto root = fresh();
  auto catalog = value(cat::Catalog::open(root, options()));
  auto& store = catalog.job_store();
  c::JobSpec spec;
  spec.type = "dev.probe";
  auto job = value(store.enqueue(spec));
  c::JobPatch release;
  release.release_attempt = true;
  auto invalid = store.transition(job.job_id, c::JobState::queued, c::JobState::paused, release);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().message_id, "am.jobs.transition_invalid");
  value(store.transition(job.job_id, c::JobState::queued, c::JobState::running));
  EXPECT_EQ(value(store.transition(job.job_id, c::JobState::running, c::JobState::queued, release)).attempt, 0);
  auto connection = am::db::Connection::borrow(catalog.library().database(am::library::DatabaseKind::state));
  value(connection.exec("UPDATE jobs SET state='running',attempt=0 WHERE job_id='" + job.job_id + "'"));
  EXPECT_EQ(value(store.transition(job.job_id, c::JobState::running, c::JobState::queued, release)).attempt, 0);
  value(store.transition(job.job_id, c::JobState::queued, c::JobState::running));
  c::JobPatch question;
  question.decision_json = "{\"kind\":\"test\",\"options\":[\"yes\"]}";
  value(store.transition(job.job_id, c::JobState::running, c::JobState::needs_decision, question));
  c::JobPatch answer;
  answer.decision_json = "{\"kind\":\"test\",\"options\":[\"yes\"],\"answer\":{\"option\":\"yes\"}}";
  EXPECT_EQ(value(store.transition(job.job_id, c::JobState::needs_decision, c::JobState::queued, answer)).decision_json,
            answer.decision_json);
  value(store.transition(job.job_id, c::JobState::queued, c::JobState::running));
  c::JobPatch finished;
  finished.clear_decision = true;
  EXPECT_FALSE(value(store.transition(job.job_id, c::JobState::running, c::JobState::failed, finished)).decision_json);
}
