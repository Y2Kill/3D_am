#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <condition_variable>
#include <random>
#include <thread>

#include "am/jobs/executor.hpp"
#include "job_test_store.hpp"
#ifdef AM_JOBS_STORAGE_TESTS
#include "am/storage/db/job_store.hpp"
#include "am/storage/library/library.hpp"
#endif

namespace {
namespace j = am::jobs;
namespace c = am::contracts;
using S = c::JobState;
template <class T>
T value(am::Result<T> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
  return std::move(*r);
}
void value(am::Result<void> r) {
  if (!r) throw std::runtime_error(am::to_log_string(r.error()));
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned count = 0;
  void signal() {
    const std::lock_guard lock(mutex);
    ++count;
    changed.notify_all();
  }
  bool wait(unsigned n = 1) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(15), [&] { return count >= n; });
  }
};
j::ResourceLimits limits(unsigned jobs = 1) { return {jobs, 1, 1, 1, 1, 1, 1, 1}; }
std::array<j::ResourceRequest, 1> cpu{{{j::ResourceKind::cpu, ""}}};
struct FunctionHandler final : j::JobHandler {
  std::function<j::JobOutcome(j::JobContext&)> function;
  explicit FunctionHandler(std::function<j::JobOutcome(j::JobContext&)> f) : function(std::move(f)) {}
  j::JobOutcome run(j::JobContext& context) override { return function(context); }
};
void add(j::HandlerRegistry& registry, const std::string& type, std::function<j::JobOutcome(j::JobContext&)> function) {
  value(registry.add(
      type, [function = std::move(function)](const auto&) { return std::make_unique<FunctionHandler>(function); }));
}
struct Observer final : j::JobObserver {
  std::function<void(const j::JobEvent&)> callback;
  std::mutex mutex;
  std::vector<j::JobEvent> events;
  bool threw = false;
  void on_event(const j::JobEvent& event) noexcept override {
    try {
      {
        const std::lock_guard lock(mutex);
        events.push_back(event);
      }
      if (callback) callback(event);
    } catch (...) {
      threw = true;
    }
  }
};
class JobsExecutor : public testing::TestWithParam<bool> {
 protected:
#ifdef AM_JOBS_STORAGE_TESTS
  std::optional<am::library::Library> library;
#endif
  std::unique_ptr<c::IJobStore> backing;
  std::unique_ptr<job_tests::CountingStore> store;
  void SetUp() override {
#ifdef AM_JOBS_STORAGE_TESTS
    if (GetParam()) {
      const auto root = std::filesystem::path(AM_LIBRARY_TEST_ROOT) / am::Uuid::v4().to_string();
      value(am::library::Library::init(root));
      am::library::OpenOptions options;
      options.capabilities.emplace();
      options.capabilities->mode = am::repository::StorageMode::full;
      library.emplace(value(am::library::Library::open(root, options)));
      backing = std::make_unique<am::db::StateJobStore>(library->database(am::library::DatabaseKind::state));
    } else
#endif
      backing = std::make_unique<job_tests::MemoryStore>();
    store = std::make_unique<job_tests::CountingStore>(*backing);
  }
  c::JobRecord enqueue(std::string type = "test.ok", int priority = 0, std::string params = "{}",
                       std::optional<std::string> parent = {}) {
    c::JobSpec spec;
    spec.type = std::move(type);
    spec.priority = priority;
    spec.params_json = std::move(params);
    spec.parent_id = std::move(parent);
    return value(store->enqueue(spec));
  }
  c::JobRecord get(const std::string& id) { return *value(store->get(id)); }
  j::ExecutorOptions options(unsigned jobs = 1) {
    j::ExecutorOptions out;
    out.limits = limits(jobs);
    out.retry_interval = std::chrono::milliseconds(0);
    return out;
  }
};
}  // namespace
TEST(JobsResources, ProfilesAreHypothesesAndHardwareZeroIsSupported) {
  RecordProperty("req", "K-115");
  const auto economy = j::profile_limits(j::Profile::economy, 8), balanced = j::profile_limits(j::Profile::balanced, 8),
             performance = j::profile_limits(j::Profile::performance, 8);
  EXPECT_EQ(economy.jobs, 1u);
  EXPECT_EQ(economy.cpu, 1u);
  EXPECT_EQ(economy.external_process, 1u);
  EXPECT_EQ(economy.io_ssd, 1u);
  EXPECT_EQ(balanced.jobs, 2u);
  EXPECT_EQ(balanced.cpu, 4u);
  EXPECT_EQ(balanced.external_process, 1u);
  EXPECT_EQ(balanced.io_ssd, 2u);
  EXPECT_EQ(performance.jobs, 4u);
  EXPECT_EQ(performance.cpu, 7u);
  EXPECT_EQ(performance.external_process, 2u);
  EXPECT_EQ(performance.io_ssd, 4u);
  for (const auto profile : {j::Profile::economy, j::Profile::balanced, j::Profile::performance, j::Profile::custom}) {
    const auto l = j::profile_limits(profile, 0);
    EXPECT_GE(l.jobs, 1u);
    EXPECT_GE(l.cpu, 1u);
    EXPECT_EQ(l.io_hdd, 1u);
    EXPECT_EQ(l.io_removable, 1u);
  }
  EXPECT_EQ(j::profile_limits(j::Profile::custom, 8).cpu, balanced.cpu);
  EXPECT_EQ(performance.io_network, 2u);
  EXPECT_EQ(performance.io_unknown, 2u);
}
TEST(JobsResources, EveryZeroLimitIsRejected) {
  RecordProperty("req", "K-115");
  for (unsigned i = 0; i < 8; ++i) {
    auto l = limits();
    std::array<unsigned*, 8> fields{&l.jobs,   &l.cpu,        &l.external_process, &l.io_hdd,
                                    &l.io_ssd, &l.io_network, &l.io_removable,     &l.io_unknown};
    *fields[i] = 0;
    j::ResourceManager resources(l);
    auto lease = resources.acquire(cpu, false, 0);
    ASSERT_FALSE(lease);
    EXPECT_EQ(lease.error().message_id, "am.jobs.limits_invalid");
  }
}
TEST(JobsResources, WaitingOrderIsInteractiveThenPriorityThenFifo) {
  RecordProperty("req", "K-115");
  Gate registered;
  std::mutex mutex;
  std::vector<int> order;
  j::ResourceManager resources(limits(), [&] { registered.signal(); });
  std::optional<j::Lease> held(value(resources.acquire(cpu, false, 0)));
  std::vector<std::jthread> workers;
  auto start = [&](int id, bool interactive, int priority) {
    workers.emplace_back([&, id, interactive, priority](std::stop_token stop) {
      auto lease = resources.acquire(cpu, interactive, priority, stop);
      if (lease) {
        const std::lock_guard lock(mutex);
        order.push_back(id);
      }
    });
  };
  start(1, false, 1);
  ASSERT_TRUE(registered.wait(2));
  start(2, false, 9);
  ASSERT_TRUE(registered.wait(3));
  start(3, true, -1);
  ASSERT_TRUE(registered.wait(4));
  start(4, false, 9);
  ASSERT_TRUE(registered.wait(5));
  const auto snapshot = resources.snapshot();
  ASSERT_EQ(snapshot.waiting.size(), 4u);
  EXPECT_TRUE(snapshot.waiting.front().interactive);
  held.reset();
  for (auto& worker : workers) worker.join();
  EXPECT_EQ(order, (std::vector<int>{3, 2, 4, 1}));
}
TEST(JobsResources, DevicesAndReadWritePoolsAreIndependent) {
  RecordProperty("req", "K-115");
  Gate registered;
  j::ResourceManager resources(limits(), [&] { registered.signal(); });
  resources.set_media("A", j::MediaKind::hdd);
  resources.set_media("B", j::MediaKind::ssd);
  const std::array<j::ResourceRequest, 1> readA{{{j::ResourceKind::read, "A"}}};
  std::optional<j::Lease> first(value(resources.acquire(readA, false, 0)));
  Gate entered, release, acquired;
  std::jthread worker([&](std::stop_token stop) {
    const std::array<j::ResourceRequest, 2> request{{{j::ResourceKind::read, "B"}, {j::ResourceKind::write, "A"}}};
    auto second = resources.acquire(request, false, 0, stop);
    if (second) {
      entered.signal();
      (void)release.wait();
    }
  });
  ASSERT_TRUE(entered.wait());
  const auto snapshot = resources.snapshot();
  unsigned occupied = 0;
  for (const auto& usage : snapshot.usage) occupied += usage.occupied;
  EXPECT_EQ(occupied, 3u);
  std::jthread waiter([&](std::stop_token stop) {
    auto lease = resources.acquire(readA, false, 0, stop);
    if (lease) acquired.signal();
  });
  ASSERT_TRUE(registered.wait(3));
  EXPECT_EQ(resources.snapshot().waiting.size(), 1u);
  first.reset();
  EXPECT_TRUE(acquired.wait());
  waiter.join();
  release.signal();
  worker.join();
}
TEST(JobsResources, AtomicSetsCannotDeadlockEvenInOppositeOrder) {
  RecordProperty("req", "K-115");
  j::ResourceManager resources(limits());
  std::atomic<unsigned> completed = 0;
  Gate finished;
  std::barrier start(7);
  std::vector<std::jthread> workers;
  for (unsigned i = 0; i < 6; ++i)
    workers.emplace_back([&, i](std::stop_token stop) {
      std::mt19937 random(42 + i);
      start.arrive_and_wait();
      for (unsigned n = 0; n < 32; ++n) {
        std::array<j::ResourceRequest, 2> request{{{j::ResourceKind::read, "A"}, {j::ResourceKind::write, "B"}}};
        if (random() & 1) std::swap(request[0], request[1]);
        auto lease = resources.acquire(request, false, 0, stop);
        if (!lease) return;
        ++completed;
        finished.signal();
      }
    });
  start.arrive_and_wait();
  const bool complete = finished.wait(192);
  if (!complete)
    for (auto& worker : workers) worker.request_stop();
  for (auto& worker : workers) worker.join();
  EXPECT_TRUE(complete);
  EXPECT_EQ(completed, 192u);
  for (const auto& usage : resources.snapshot().usage) EXPECT_EQ(usage.occupied, 0u);
}
TEST(JobsResources, CancelledWaitHasNoPartialLeaseAndNestedKindIsRejected) {
  RecordProperty("req", "K-115");
  Gate registered;
  j::ResourceManager resources(limits(), [&] { registered.signal(); });
  std::optional<j::Lease> held(value(resources.acquire(cpu, false, 0)));
  auto nested = resources.acquire(cpu, false, 0);
  ASSERT_FALSE(nested);
  EXPECT_EQ(nested.error().message_id, "am.jobs.lease_nested");
  std::optional<am::Result<j::Lease>> result;
  std::jthread worker([&](std::stop_token stop) {
    std::array<j::ResourceRequest, 2> requests{{{j::ResourceKind::cpu, ""}, {j::ResourceKind::read, "A"}}};
    result.emplace(resources.acquire(requests, false, 0, stop));
  });
  ASSERT_TRUE(registered.wait(2));
  worker.request_stop();
  worker.join();
  ASSERT_TRUE(result);
  ASSERT_FALSE(*result);
  EXPECT_EQ(result->error().code, am::Errc::cancelled);
  for (const auto& usage : resources.snapshot().usage) {
    if (usage.kind == j::ResourceKind::read) {
      EXPECT_EQ(usage.occupied, 0u);
    }
  }
  held.reset();
  auto again = resources.acquire(cpu, false, 0);
  ASSERT_TRUE(again);
}
TEST_P(JobsExecutor, SuccessPriorityPositionAndDuplicateRegistration) {
  RecordProperty("req", "K-121|NFR-14.4.3");
  j::HandlerRegistry handlers;
  add(handlers, "test.ok", [](auto&) { return j::JobOutcome::success("{\"answer\":42}"); });
  auto duplicate = handlers.add("test.ok", [](const auto&) {
    return std::make_unique<FunctionHandler>([](auto&) { return j::JobOutcome::success(); });
  });
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().message_id, "am.jobs.type_duplicate");
  const auto a = enqueue(), b = enqueue("test.ok", 5), cjob = enqueue("test.ok", 5);
  value(store->move_before(cjob.job_id, b.job_id));
  Observer observer;
  auto config = options();
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config, &observer);
  const auto summary = value(executor.run_until_idle());
  EXPECT_EQ(summary.processed, 3u);
  EXPECT_EQ(summary.succeeded, 3u);
  EXPECT_EQ(summary.failed, 0u);
  std::vector<std::string> starts;
  for (const auto& event : observer.events)
    if (event.kind == j::EventKind::started) starts.push_back(event.job.job_id);
  EXPECT_EQ(starts, (std::vector<std::string>{cjob.job_id, b.job_id, a.job_id}));
  EXPECT_EQ(get(a.job_id).result_json, std::optional<std::string>("{\"answer\":42}"));
}
TEST_P(JobsExecutor, UnknownTypeExceptionAndFailureAreIsolated) {
  RecordProperty("req", "K-121");
  j::HandlerRegistry handlers;
  add(handlers, "test.throw", [](auto&) -> j::JobOutcome { throw std::runtime_error("injected failure"); });
  add(handlers, "test.fail",
      [](auto&) { return j::JobOutcome::failure(am::Error::make(am::Errc::io_error, "Injected").id("test.failed")); });
  add(handlers, "test.ok", [](auto&) { return j::JobOutcome::success(); });
  const auto unknown = enqueue("test.unknown"), thrown = enqueue("test.throw"), failed = enqueue("test.fail"),
             good = enqueue();
  auto config = options(2);
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  const auto summary = value(executor.run_until_idle());
  EXPECT_EQ(summary.failed, 3u);
  EXPECT_EQ(get(good.job_id).state, S::succeeded);
  EXPECT_NE(get(unknown.job_id).error_json->find("am.jobs.type_unknown"), std::string::npos);
  EXPECT_NE(get(thrown.job_id).error_json->find("am.jobs.handler_exception"), std::string::npos);
  EXPECT_NE(get(failed.job_id).error_json->find("test.failed"), std::string::npos);
}
TEST_P(JobsExecutor, InteractiveReserveStartsWhileBackgroundSlotsAreOccupied) {
  RecordProperty("req", "K-115");
  Gate background, interactive, release;
  std::atomic<unsigned> active = 0, maximum = 0;
  j::HandlerRegistry handlers;
  add(handlers, "test.block", [&](auto&) {
    const auto n = ++active;
    auto previous = maximum.load();
    while (n > previous && !maximum.compare_exchange_weak(previous, n)) {
    }
    background.signal();
    const bool ready = release.wait();
    --active;
    return ready ? j::JobOutcome::success() : j::JobOutcome::failure(job_tests::error("test.timeout"));
  });
  add(handlers, "test.interactive", [&](auto&) {
    interactive.signal();
    return j::JobOutcome::success();
  });
  for (unsigned n = 0; n < 8; ++n) enqueue("test.block");
  auto config = options(2);
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  std::optional<am::Result<j::RunSummary>> result;
  std::jthread run([&] { result.emplace(executor.run_until_idle()); });
  ASSERT_TRUE(background.wait(2));
  const auto foreground = enqueue("test.interactive", j::interactive_priority);
  executor.wake();
  ASSERT_TRUE(interactive.wait());
  EXPECT_EQ(maximum, 2u);
  release.signal();
  run.join();
  ASSERT_TRUE(result);
  ASSERT_TRUE(*result);
  EXPECT_EQ(maximum, 2u);
  EXPECT_EQ(get(foreground.job_id).state, S::succeeded);
}
TEST_P(JobsExecutor, PauseCancelAndShutdownWaitForSafePointAndReleaseAttempt) {
  RecordProperty("req", "K-115|NFR-14.4.3");
  for (const auto reason : {j::StopReason::pause, j::StopReason::cancel, j::StopReason::shutdown}) {
    Gate entered, release;
    j::HandlerRegistry handlers;
    add(handlers, "test.safe", [&](auto& context) {
      entered.signal();
      if (!release.wait()) return j::JobOutcome::failure(job_tests::error("test.timeout"));
      std::mutex mutex;
      std::condition_variable_any stopped;
      std::unique_lock lock(mutex);
      stopped.wait_until(lock, context.stop(), std::chrono::steady_clock::now() + std::chrono::seconds(15),
                         [] { return false; });
      if (!context.stop().stop_requested()) {
        ADD_FAILURE() << "Executor did not deliver the stop request before the test deadline";
        return j::JobOutcome::failure(job_tests::error("test.stop_timeout"));
      }
      return j::JobOutcome::interrupted();
    });
    const auto job = enqueue("test.safe");
    auto config = options();
    j::ResourceManager resources(config.limits);
    j::Executor executor(*store, resources, handlers, config);
    std::stop_source stop;
    std::optional<am::Result<j::RunSummary>> result;
    std::jthread run([&] { result.emplace(executor.run_until_idle(stop.get_token())); });
    ASSERT_TRUE(entered.wait());
    if (reason == j::StopReason::pause)
      value(executor.pause(job.job_id));
    else if (reason == j::StopReason::cancel)
      value(executor.cancel(job.job_id));
    else
      stop.request_stop();
    EXPECT_EQ(get(job.job_id).state, S::running);
    release.signal();
    run.join();
    ASSERT_TRUE(result);
    ASSERT_TRUE(*result);
    EXPECT_EQ(get(job.job_id).state, reason == j::StopReason::pause    ? S::paused
                                     : reason == j::StopReason::cancel ? S::cancelled
                                                                       : S::queued);
    EXPECT_EQ(get(job.job_id).attempt, reason == j::StopReason::shutdown ? 0 : 1);
    if (reason == j::StopReason::shutdown) {
      EXPECT_TRUE((*result)->interrupted);
      value(executor.cancel(job.job_id));
    }
  }
}
TEST_P(JobsExecutor, CompletedWorkWinsOverLateShutdown) {
  RecordProperty("req", "NFR-14.4.3");
  Gate entered, release;
  j::HandlerRegistry handlers;
  add(handlers, "test.safe", [&](auto&) {
    entered.signal();
    (void)release.wait();
    return j::JobOutcome::success();
  });
  const auto job = enqueue("test.safe");
  auto config = options();
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  std::stop_source stop;
  std::jthread run([&] { value(executor.run_until_idle(stop.get_token())); });
  ASSERT_TRUE(entered.wait());
  stop.request_stop();
  release.signal();
  run.join();
  EXPECT_EQ(get(job.job_id).state, S::succeeded);
}
TEST_P(JobsExecutor, DecisionAnswerSurvivesRestartAndInvalidChoiceDoesNotWrite) {
  RecordProperty("req", "NFR-14.4.3|K-121");
  j::HandlerRegistry handlers;
  std::string answer;
  add(handlers, "test.ask", [&](auto& context) {
    auto selected = context.answer();
    if (!selected) return j::JobOutcome::ask({"question", {"continue", "stop"}, "{\"n\":1e9999}"});
    answer = *selected;
    return j::JobOutcome::success();
  });
  const auto job = enqueue("test.ask");
  auto config = options();
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  EXPECT_EQ(value(executor.run_until_idle()).attention, 1u);
  auto invalid = executor.answer(job.job_id, "other");
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().message_id, "am.jobs.answer_invalid");
  EXPECT_EQ(get(job.job_id).state, S::needs_decision);
  value(executor.answer(job.job_id, "continue"));
  EXPECT_NE(get(job.job_id).decision_json->find("1e9999"), std::string::npos);
  EXPECT_EQ(value(executor.run_until_idle()).succeeded, 1u);
  EXPECT_EQ(answer, "continue");
  EXPECT_FALSE(get(job.job_id).decision_json);
}
TEST_P(JobsExecutor, BoundedAutomaticRetriesBecomeResourceError) {
  RecordProperty("req", "K-115|K-121");
  j::HandlerRegistry handlers;
  unsigned calls = 0;
  add(handlers, "test.wait", [&](auto&) {
    ++calls;
    return j::JobOutcome::failure(am::Error::make(am::Errc::insufficient_space, "Injected").id("test.space"));
  });
  const auto job = enqueue("test.wait");
  auto config = options();
  config.auto_retries = 3;
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  EXPECT_EQ(value(executor.run_until_idle()).attention, 1u);
  EXPECT_EQ(calls, 4u);
  EXPECT_EQ(get(job.job_id).state, S::resource_error);
  EXPECT_EQ(get(job.job_id).attempt, 4);
  value(executor.retry(job.job_id));
  value(executor.cancel(job.job_id));
}
TEST_P(JobsExecutor, ObserverCanPauseWithoutExecutorLocks) {
  RecordProperty("req", "K-115");
  j::HandlerRegistry handlers;
  add(handlers, "test.safe", [](auto& context) {
    return context.stop().stop_requested() ? j::JobOutcome::interrupted() : j::JobOutcome::success();
  });
  const auto job = enqueue("test.safe");
  auto config = options();
  j::ResourceManager resources(config.limits);
  Observer observer;
  j::Executor executor(*store, resources, handlers, config, &observer);
  observer.callback = [&](const auto& event) {
    if (event.kind == j::EventKind::started) value(executor.pause(event.job.job_id));
  };
  value(executor.run_until_idle());
  EXPECT_FALSE(observer.threw);
  EXPECT_EQ(get(job.job_id).state, S::paused);
}
TEST_P(JobsExecutor, ProgressThrottleStageAndFinalFlushUseInjectedClock) {
  RecordProperty("req", "NFR-14.4.3");
  j::HandlerRegistry handlers;
  std::atomic<int> ticks = 0;
  Observer observer;
  add(handlers, "test.report", [&](auto& context) {
    context.report({0, 10, "files", "read"});
    for (unsigned n = 1; n <= 8; ++n) {
      ticks = static_cast<int>(n);
      context.report({n, 10, "files", "read"});
    }
    context.report({9, 10, "files", "write"});
    ticks = 10;
    context.report({10, 10, "files", "write"});
    return j::JobOutcome::success();
  });
  const auto job = enqueue("test.report");
  auto config = options();
  config.report_interval = std::chrono::milliseconds(100);
  config.clock = [&] { return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(ticks.load()); };
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config, &observer);
  value(executor.run_until_idle());
  EXPECT_EQ(store->reports, 3u);
  EXPECT_EQ(get(job.job_id).progress.done, 10u);
  EXPECT_EQ(get(job.job_id).progress.stage, "write");
  bool rate = false;
  for (const auto& event : observer.events)
    if (event.kind == j::EventKind::progress && event.rate) rate = true;
  EXPECT_TRUE(rate);
}
TEST_P(JobsExecutor, ThreeGracefulShutdownsDoNotExhaustCrashRecovery) {
  RecordProperty("req", "NFR-14.4.3");
  j::HandlerRegistry handlers;
  add(handlers, "test.stop", [](auto& context) {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unique_lock lock(mutex);
    changed.wait_until(lock, context.stop(), std::chrono::steady_clock::now() + std::chrono::seconds(15),
                       [] { return false; });
    return context.stop().stop_requested() ? j::JobOutcome::interrupted()
                                           : j::JobOutcome::failure(job_tests::error("test.timeout"));
  });
  const auto job = enqueue("test.stop");
  auto config = options();
  j::ResourceManager resources(config.limits);
  Observer observer;
  j::Executor executor(*store, resources, handlers, config, &observer);
  for (unsigned n = 0; n < 3; ++n) {
    std::stop_source stop;
    observer.callback = [&](const auto& event) {
      if (event.kind == j::EventKind::started) stop.request_stop();
    };
    const auto result = value(executor.run_until_idle(stop.get_token()));
    EXPECT_TRUE(result.interrupted);
    EXPECT_EQ(get(job.job_id).state, S::queued);
    EXPECT_EQ(get(job.job_id).attempt, 0);
  }
  value(store->transition(job.job_id, S::queued, S::running));
#ifdef AM_JOBS_STORAGE_TESTS
  if (GetParam()) {
    auto& persistent = dynamic_cast<am::db::StateJobStore&>(*backing);
    const auto recovered = value(persistent.recover_running());
    EXPECT_EQ(recovered.requeued, (std::vector<std::string>{job.job_id}));
    EXPECT_TRUE(recovered.failed.empty());
  } else
#endif
    value(store->transition(job.job_id, S::running, S::queued));
  EXPECT_EQ(get(job.job_id).attempt, 1);
  EXPECT_EQ(get(job.job_id).state, S::queued);
}
TEST_P(JobsExecutor, StartupWaitingResourceCountsAsFirstRetry) {
  RecordProperty("req", "K-115|NFR-14.4.3");
  const auto job = enqueue("test.wait");
  value(store->transition(job.job_id, S::queued, S::running));
  value(store->transition(job.job_id, S::running, S::waiting_resources));
  unsigned calls = 0;
  j::HandlerRegistry handlers;
  add(handlers, "test.wait", [&](auto&) {
    ++calls;
    j::JobOutcome result;
    result.kind = j::OutcomeKind::waiting_resources;
    result.error = am::Error::make(am::Errc::insufficient_space, "Injected").id("test.space");
    return result;
  });
  auto config = options();
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  value(executor.run_until_idle());
  EXPECT_EQ(calls, 3u);
  EXPECT_EQ(get(job.job_id).state, S::resource_error);
}
TEST_P(JobsExecutor, ErrorCodesAndExplicitOutcomesHaveRequiredPrecedence) {
  RecordProperty("req", "K-115|K-121");
  for (const auto code : {am::Errc::io_error, am::Errc::insufficient_space, am::Errc::resource_exhausted,
                          am::Errc::read_only, am::Errc::locked, am::Errc::cancelled}) {
    j::HandlerRegistry handlers;
    add(handlers, "test.error",
        [=](auto&) { return j::JobOutcome::failure(am::Error::make(code, "Injected").id("test.error")); });
    const auto job = enqueue("test.error");
    auto config = options();
    config.auto_retries = 0;
    j::ResourceManager resources(config.limits);
    j::Executor executor(*store, resources, handlers, config);
    value(executor.run_until_idle());
    const auto expected = code == am::Errc::read_only || code == am::Errc::locked ? S::cannot_continue_safely
                          : code == am::Errc::cancelled                           ? S::cancelled
                          : code == am::Errc::insufficient_space || code == am::Errc::resource_exhausted
                              ? S::resource_error
                              : S::failed;
    EXPECT_EQ(get(job.job_id).state, expected);
  }
  j::HandlerRegistry handlers;
  add(handlers, "test.explicit", [](auto&) {
    j::JobOutcome result;
    result.kind = j::OutcomeKind::resource_error;
    result.error = am::Error::make(am::Errc::locked, "Explicit");
    return result;
  });
  const auto job = enqueue("test.explicit");
  auto config = options();
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  value(executor.run_until_idle());
  EXPECT_EQ(get(job.job_id).state, S::resource_error);
}
TEST_P(JobsExecutor, JobProfileDoesNotReplaceExecutorLimitsAndUnknownTotalStaysUnknown) {
  RecordProperty("req", "K-115|NFR-14.4.3");
  j::HandlerRegistry handlers;
  j::Profile selected = j::Profile::balanced;
  j::ResourceManager* manager = nullptr;
  unsigned observed_limit = 0;
  add(handlers, "test.profile", [&](auto& context) {
    selected = context.profile();
    auto lease = value(context.acquire(cpu));
    for (const auto& usage : manager->snapshot().usage)
      if (usage.kind == j::ResourceKind::cpu) observed_limit = usage.limit;
    context.report({1, {}, "items", "work"});
    return j::JobOutcome::success();
  });
  const auto job = enqueue("test.profile", 0, "{\"resource_profile\":\"performance\"}");
  auto config = options();
  config.profile = j::Profile::economy;
  j::ResourceManager resources(config.limits);
  manager = &resources;
  j::Executor executor(*store, resources, handlers, config);
  value(executor.run_until_idle());
  EXPECT_EQ(selected, j::Profile::performance);
  EXPECT_EQ(observed_limit, 1u);
  EXPECT_FALSE(get(job.job_id).progress.total);
  EXPECT_EQ(get(job.job_id).progress.done, 1u);
  const auto invalid = enqueue("test.profile", 0, "{\"resource_profile\":\"invalid\"}");
  value(executor.run_until_idle());
  EXPECT_EQ(get(invalid.job_id).state, S::failed);
  EXPECT_NE(get(invalid.job_id).error_json->find("am.jobs.params_invalid"), std::string::npos);
}
TEST_P(JobsExecutor, ParentReleasesSlotAndReentersAfterAllChildrenSettle) {
  RecordProperty("req", "K-121|NFR-14.4.3");
  unsigned parent_calls = 0;
  j::HandlerRegistry handlers;
  add(handlers, "test.child", [](auto& context) {
    return context.record().params_json == "{\"fail\":true}"
               ? j::JobOutcome::failure(job_tests::error("test.child_failed"))
               : j::JobOutcome::success();
  });
  add(handlers, "test.parent", [&](auto& context) {
    ++parent_calls;
    const auto children = context.children();
    if (children.records.empty()) {
      for (unsigned n = 0; n < 3; ++n) {
        c::JobSpec spec;
        spec.type = "test.child";
        spec.params_json = n == 1 ? "{\"fail\":true}" : "{}";
        value(context.enqueue_child(spec));
      }
      return j::JobOutcome::await_children();
    }
    EXPECT_TRUE(children.settled());
    return j::JobOutcome::success("{\"children\":3}");
  });
  const auto parent = enqueue("test.parent");
  auto config = options();
  j::ResourceManager resources(config.limits);
  Observer observer;
  j::Executor executor(*store, resources, handlers, config, &observer);
  const auto result = value(executor.run_until_idle());
  EXPECT_EQ(parent_calls, 2u);
  EXPECT_EQ(result.processed, 4u);
  EXPECT_EQ(result.succeeded, 3u);
  EXPECT_EQ(result.failed, 1u);
  EXPECT_EQ(get(parent.job_id).attempt, 1);
  EXPECT_EQ(get(parent.job_id).state, S::succeeded);
  unsigned parent_starts = 0;
  for (const auto& event : observer.events)
    if (event.kind == j::EventKind::started && event.job.job_id == parent.job_id) ++parent_starts;
  EXPECT_EQ(parent_starts, 1u);
}
TEST_P(JobsExecutor, ChildDecisionHoldsParentAndRestartIsIdempotent) {
  RecordProperty("req", "NFR-14.4.3|K-121");
  j::HandlerRegistry handlers;
  add(handlers, "test.ask", [](auto& context) {
    return context.answer() ? j::JobOutcome::success() : j::JobOutcome::ask({"choice", {"continue"}, "{}"});
  });
  add(handlers, "test.parent", [](auto& context) {
    const auto children = context.children();
    if (children.records.empty()) {
      c::JobSpec child;
      child.type = "test.ask";
      value(context.enqueue_child(child));
      return j::JobOutcome::await_children();
    }
    return children.settled() ? j::JobOutcome::success() : j::JobOutcome::await_children();
  });
  const auto parent = enqueue("test.parent");
  auto config = options();
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  EXPECT_EQ(value(executor.run_until_idle()).attention, 1u);
  EXPECT_EQ(get(parent.job_id).state, S::running);
  c::JobFilter filter;
  filter.parent_id = parent.job_id;
  auto children = value(store->list(filter));
  ASSERT_EQ(children.size(), 1u);
  value(executor.answer(children[0].job_id, "continue"));
  value(executor.run_until_idle());
  EXPECT_EQ(value(store->list(filter)).size(), 1u);
  EXPECT_EQ(get(parent.job_id).state, S::succeeded);
}
TEST_P(JobsExecutor, ParentPauseResumeAndCancelCascadeAtSafePoints) {
  RecordProperty("req", "NFR-14.4.3|K-121");
  for (const bool cancel : {false, true}) {
    Gate entered, release;
    j::HandlerRegistry handlers;
    add(handlers, "test.child", [&](auto& context) {
      entered.signal();
      if (!release.wait()) return j::JobOutcome::failure(job_tests::error("test.timeout"));
      return context.stop().stop_requested() ? j::JobOutcome::interrupted() : j::JobOutcome::success();
    });
    add(handlers, "test.parent", [](auto& context) {
      auto children = context.children();
      if (children.records.empty()) {
        for (unsigned n = 0; n < 2; ++n) {
          c::JobSpec child;
          child.type = "test.child";
          value(context.enqueue_child(child));
        }
        return j::JobOutcome::await_children();
      }
      return children.settled() ? j::JobOutcome::success() : j::JobOutcome::await_children();
    });
    const auto parent = enqueue("test.parent");
    auto config = options();
    j::ResourceManager resources(config.limits);
    j::Executor executor(*store, resources, handlers, config);
    std::optional<am::Result<j::RunSummary>> result;
    std::jthread run([&] { result.emplace(executor.run_until_idle()); });
    ASSERT_TRUE(entered.wait());
    if (cancel)
      value(executor.cancel(parent.job_id));
    else
      value(executor.pause(parent.job_id));
    release.signal();
    run.join();
    ASSERT_TRUE(result);
    ASSERT_TRUE(*result);
    c::JobFilter filter;
    filter.parent_id = parent.job_id;
    for (const auto& child : value(store->list(filter))) EXPECT_EQ(child.state, cancel ? S::cancelled : S::paused);
    EXPECT_EQ(get(parent.job_id).state, cancel ? S::cancelled : S::paused);
    if (!cancel) {
      value(executor.resume(parent.job_id));
      value(executor.run_until_idle());
      EXPECT_EQ(get(parent.job_id).state, S::succeeded);
      EXPECT_EQ(value(store->list(filter)).size(), 2u);
    }
  }
}
TEST_P(JobsExecutor, ParentRetryRetriesFailedChildrenWithoutDuplicatingSuccessfulOnes) {
  RecordProperty("req", "K-121|NFR-14.4.3");
  bool fail = true;
  j::HandlerRegistry handlers;
  add(handlers, "test.child",
      [&](auto&) { return fail ? j::JobOutcome::failure(job_tests::error("test.failed")) : j::JobOutcome::success(); });
  add(handlers, "test.ok", [](auto&) { return j::JobOutcome::success(); });
  add(handlers, "test.parent", [](auto& context) {
    auto children = context.children();
    if (children.records.empty()) {
      for (const auto* type : {"test.child", "test.ok"}) {
        c::JobSpec child;
        child.type = type;
        value(context.enqueue_child(child));
      }
      return j::JobOutcome::await_children();
    }
    if (!children.settled()) return j::JobOutcome::await_children();
    return children.counts[S::failed] ? j::JobOutcome::failure(job_tests::error("test.failed"))
                                      : j::JobOutcome::success();
  });
  const auto parent = enqueue("test.parent");
  auto config = options();
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  value(executor.run_until_idle());
  EXPECT_EQ(get(parent.job_id).state, S::failed);
  fail = false;
  value(executor.retry(parent.job_id));
  value(executor.run_until_idle());
  EXPECT_EQ(get(parent.job_id).state, S::succeeded);
  c::JobFilter filter;
  filter.parent_id = parent.job_id;
  const auto children = value(store->list(filter));
  EXPECT_EQ(children.size(), 2u);
  for (const auto& child : children) {
    EXPECT_EQ(child.state, S::succeeded);
    EXPECT_EQ(child.attempt, child.type == "test.ok" ? 1 : 2);
  }
}
TEST_P(JobsExecutor, RecoveredParentUsesExistingChildrenAndNestedChildCreationIsRejected) {
  RecordProperty("req", "NFR-14.4.3");
  j::HandlerRegistry handlers;
  const auto parent = enqueue("test.parent");
  const auto child = enqueue("test.child", 0, "{}", parent.job_id);
  value(store->transition(parent.job_id, S::queued, S::running));
  value(store->transition(parent.job_id, S::running, S::queued));
  add(handlers, "test.child", [](auto& context) {
    c::JobSpec nested;
    nested.type = "test.child";
    auto rejected = context.enqueue_child(nested);
    EXPECT_FALSE(rejected);
    return j::JobOutcome::success();
  });
  add(handlers, "test.parent", [](auto& context) {
    return context.children().settled() ? j::JobOutcome::success() : j::JobOutcome::await_children();
  });
  auto config = options();
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  value(executor.run_until_idle());
  EXPECT_EQ(get(parent.job_id).state, S::succeeded);
  EXPECT_EQ(get(child.job_id).state, S::succeeded);
  EXPECT_EQ(value(store->list()).size(), 2u);
}
TEST_P(JobsExecutor, LateChildCreatedAfterCancelIsAlsoCancelled) {
  RecordProperty("req", "K-121|NFR-14.4.3");
  Gate entered, release;
  j::HandlerRegistry handlers;
  add(handlers, "test.parent", [&](auto& context) {
    entered.signal();
    (void)release.wait();
    c::JobSpec child;
    child.type = "test.ok";
    value(context.enqueue_child(child));
    return j::JobOutcome::await_children();
  });
  const auto parent = enqueue("test.parent");
  auto config = options();
  j::ResourceManager resources(config.limits);
  j::Executor executor(*store, resources, handlers, config);
  std::optional<am::Result<j::RunSummary>> result;
  std::jthread run([&] { result.emplace(executor.run_until_idle()); });
  ASSERT_TRUE(entered.wait());
  value(executor.cancel(parent.job_id));
  release.signal();
  run.join();
  ASSERT_TRUE(result);
  ASSERT_TRUE(*result);
  c::JobFilter filter;
  filter.parent_id = parent.job_id;
  const auto children = value(store->list(filter));
  ASSERT_EQ(children.size(), 1u);
  EXPECT_EQ(children[0].state, S::cancelled);
}
#ifdef AM_JOBS_STORAGE_TESTS
INSTANTIATE_TEST_SUITE_P(Stores, JobsExecutor, testing::Values(false, true));
#else
INSTANTIATE_TEST_SUITE_P(Stores, JobsExecutor, testing::Values(false));
#endif
