#include "am/foundation/log.hpp"
#include "am/jobs/executor.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>

#include "json.hpp"

namespace am::jobs {
namespace {
using namespace contracts;
using Clock = std::chrono::steady_clock;
Error error(std::string id, Errc code = Errc::invalid_argument, std::string detail = {}) {
  return Error::make(code, "Job execution failed", std::move(detail)).in("jobs.executor").id(std::move(id));
}
template <class T>
T take(Result<T> r) {
  if (!r) throw r.error();
  return std::move(*r);
}
void take(Result<void> r) {
  if (!r) throw r.error();
}
template <class T, class F>
Result<T> guard(F&& f) {
  try {
    return f();
  } catch (const Error& e) {
    return e;
  } catch (const std::bad_alloc&) {
    return error("am.jobs.out_of_memory", Errc::resource_exhausted);
  } catch (const std::exception& e) {
    return error("am.jobs.executor_error", Errc::internal, e.what());
  } catch (...) {
    return error("am.jobs.executor_error", Errc::internal);
  }
}
bool settled(JobState state) { return job_terminal(state) || state == JobState::failed; }
bool attention(JobState state) {
  return state == JobState::paused || state == JobState::needs_decision || state == JobState::waiting_resources ||
         state == JobState::resource_error || state == JobState::cannot_continue_safely;
}
std::optional<std::string> selected_answer(const std::optional<std::string>& decision) {
  if (!decision) return {};
  const auto tree = json::parse_strict(*decision);
  if (!tree) return {};
  const auto* answer = detail::field(*tree, tree->nodes[0], "answer");
  if (!answer) return {};
  const auto* option = detail::field(*tree, *answer, "option");
  return option && option->kind == json::Kind::string ? std::optional(option->decoded) : std::nullopt;
}
Result<Profile> job_profile(const JobRecord& record, Profile fallback) {
  const auto tree = json::parse_strict(record.params_json);
  if (!tree) return tree.error();
  const auto* node = detail::field(*tree, tree->nodes[0], "resource_profile");
  if (!node) return fallback;
  if (node->kind == json::Kind::string) {
    if (node->decoded == "economy") return Profile::economy;
    if (node->decoded == "balanced") return Profile::balanced;
    if (node->decoded == "performance") return Profile::performance;
    if (node->decoded == "custom") return Profile::custom;
  }
  return error("am.jobs.params_invalid");
}
std::string question_json(const Decision& decision) {
  if (!json::parse_strict(decision.context_json)) throw error("am.jobs.json_invalid");
  std::string out = "{\"kind\":" + detail::quote(decision.kind) + ",\"options\":[";
  bool first = true;
  for (const auto& option : decision.options) {
    if (!first) out += ',';
    first = false;
    out += detail::quote(option);
  }
  return out + "],\"context\":" + decision.context_json + "}";
}
}  // namespace
bool ChildrenSummary::settled() const noexcept {
  return std::all_of(records.begin(), records.end(), [](const auto& r) { return jobs::settled(r.state); });
}
ChildrenSummary summarize_children(std::vector<JobRecord> records) {
  ChildrenSummary out;
  out.records = std::move(records);
  out.progress = {0, 0, "", "children"};
  bool first = true, compatible = true;
  auto sum = [](std::optional<std::uint64_t>& total, std::optional<std::uint64_t> next) {
    if (!total || !next || *next > (std::numeric_limits<std::uint64_t>::max)() - *total)
      total.reset();
    else
      *total += *next;
  };
  for (const auto& r : out.records) {
    ++out.counts[r.state];
    if (first) {
      out.progress.unit = r.progress.unit;
      first = false;
    } else if (out.progress.unit != r.progress.unit)
      compatible = false;
    sum(out.progress.done, r.progress.done);
    sum(out.progress.total, r.progress.total);
  }
  if (!compatible) {
    out.progress.done.reset();
    out.progress.total.reset();
    out.progress.unit.clear();
  }
  return out;
}
JobOutcome JobOutcome::success(std::string result) {
  JobOutcome o;
  o.result_json = std::move(result);
  return o;
}
JobOutcome JobOutcome::failure(Error e) {
  JobOutcome o;
  o.kind = OutcomeKind::failed;
  o.error = std::move(e);
  return o;
}
JobOutcome JobOutcome::ask(Decision d) {
  JobOutcome o;
  o.kind = OutcomeKind::needs_decision;
  o.decision = std::move(d);
  return o;
}
JobOutcome JobOutcome::interrupted() {
  JobOutcome o;
  o.kind = OutcomeKind::interrupted;
  return o;
}
JobOutcome JobOutcome::await_children() {
  JobOutcome o;
  o.kind = OutcomeKind::await_children;
  return o;
}
Result<void> HandlerRegistry::add(std::string type, Factory factory) {
  return guard<void>([&]() -> Result<void> {
    if (type.empty() || !factory) return error("am.jobs.type_invalid");
    if (factories_.contains(type)) return error("am.jobs.type_duplicate");
    factories_.emplace(std::move(type), std::move(factory));
    return {};
  });
}
const HandlerRegistry::Factory* HandlerRegistry::find(std::string_view type) const noexcept {
  const auto at = factories_.find(type);
  return at == factories_.end() ? nullptr : &at->second;
}
struct Execution {
  JobRecord record;
  Profile profile = Profile::balanced;
  std::stop_source stop;
  std::atomic<StopReason> reason{StopReason::none};
  std::thread worker;
  bool done = false;
  JobOutcome outcome;
  std::optional<JobProgress> pending, previous;
  Clock::time_point written{}, measured{};
  std::optional<double> rate;
  std::optional<Error> failure;
};
struct ExecutorState : std::enable_shared_from_this<ExecutorState> {
  IJobStore& store;
  ResourceManager& resources;
  const HandlerRegistry& registry;
  ExecutorOptions options;
  JobObserver* observer;
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::string, std::shared_ptr<Execution>> active;
  std::map<std::string, int> parents;
  std::map<std::string, Clock::time_point> retries;
  std::map<std::string, int> retry_counts;
  std::set<std::string> touched;
  bool running = false;
  std::uint64_t epoch = 0;
  ExecutorState(IJobStore& s, ResourceManager& r, const HandlerRegistry& h, ExecutorOptions o, JobObserver* ob)
      : store(s), resources(r), registry(h), options(std::move(o)), observer(ob) {}
  Clock::time_point now() const { return options.clock ? options.clock() : Clock::now(); }
  void emit(const JobEvent& event) noexcept {
    try {
      if(event.kind==EventKind::progress) {
        if (log::enabled(log::Level::trace)) {
          log::Scope scope({event.job.job_id, {}, {}, {}});
          std::vector<log::Field> fields{{"unit", event.job.progress.unit}, {"stage", event.job.progress.stage}};
          if(event.job.progress.done)fields.push_back({"done",*event.job.progress.done});
          if(event.job.progress.total)fields.push_back({"total",*event.job.progress.total});
          log::write(log::Record{log::Level::trace, "jobs", "job.progress", "Job progress", fields, nullptr, {}});
        }
      } else if(event.kind==EventKind::started || event.kind==EventKind::finished) {
        const auto level = event.kind == EventKind::started || event.job.state == JobState::succeeded ? log::Level::info
                           : event.job.state == JobState::failed || event.job.state == JobState::resource_error ||
                                   event.job.state == JobState::cannot_continue_safely
                               ? log::Level::error
                               : log::Level::warning;
        if (log::enabled(level)) {
          log::Scope scope({event.job.job_id, {}, {}, {}});
          log::write(level, "jobs", event.kind == EventKind::started ? "job.started" : "job.finished", "Job lifecycle",
                     {{"type", event.job.type},
                      {"attempt", static_cast<std::int64_t>(event.job.attempt)},
                      {"state", std::string(job_state_name(event.job.state))}});
        }
      }
    } catch(...) {}
    if (observer) observer->on_event(event);
  }
  JobRecord get(std::string_view id) {
    auto record = take(store.get(id));
    if (!record) throw error("am.jobs.not_found", Errc::not_found);
    return *record;
  }
  JobRecord transition(std::string_view id, JobState from, JobState to, const JobPatch& patch = {}) {
    ++epoch;
    return take(store.transition(id, from, to, patch));
  }
  void report(const std::shared_ptr<Execution>& execution, const JobProgress& progress, bool flush) {
    std::optional<JobEvent> event;
    bool failed = false;
    {
      const std::lock_guard lock(mutex);
      try {
        const auto at = now();
        const bool stage = !execution->previous || execution->previous->stage != progress.stage;
        if (execution->previous && execution->previous->done && progress.done &&
            execution->previous->unit == progress.unit && execution->previous->stage == progress.stage &&
            *progress.done >= *execution->previous->done) {
          const double seconds = std::chrono::duration<double>(at - execution->measured).count();
          if (seconds > 0) {
            const auto rate = static_cast<double>(*progress.done - *execution->previous->done) / seconds;
            execution->rate = std::isfinite(rate) ? std::optional(rate) : std::nullopt;
          }
        } else
          execution->rate.reset();
        execution->measured = at;
        execution->previous = progress;
        execution->pending = progress;
        if (flush || stage || options.report_interval.count() == 0 ||
            at - execution->written >= options.report_interval) {
          take(store.report(execution->record.job_id, progress));
          execution->written = at;
          execution->pending.reset();
          auto record = execution->record;
          record.progress = progress;
          event = JobEvent{EventKind::progress, std::move(record), execution->rate};
        }
      } catch (const Error& e) {
        execution->failure = e;
      } catch (...) {
        execution->failure = error("am.jobs.executor_error", Errc::internal);
      }
      failed = execution->failure.has_value();
    }
    if (failed) {
      auto reason = StopReason::none;
      execution->reason.compare_exchange_strong(reason, StopReason::cancel);
      execution->stop.request_stop();
    }
    if (event) emit(*event);
  }
  void launch(const std::shared_ptr<Execution>& execution, bool started) {
    if (started) emit({EventKind::started, execution->record, {}});
    auto self = shared_from_this();
    try {
      const auto inherited = log::current_context();
      execution->worker = std::thread([self, execution, inherited] {
        log::Scope outer(inherited);
        log::Scope scope({execution->record.job_id, {}, {}, {}});
        auto invoke = [&]() -> JobOutcome {
          const auto profile = job_profile(execution->record, self->options.profile);
          if (!profile) return JobOutcome::failure(profile.error());
          execution->profile = *profile;
          const auto* factory = self->registry.find(execution->record.type);
          if (!factory) return JobOutcome::failure(error("am.jobs.type_unknown"));
          auto handler = (*factory)(execution->record);
          if (!handler) throw std::runtime_error("Handler factory returned no handler");
          JobContext context(self, execution);
          return handler->run(context);
        };
        JobOutcome outcome;
        try {
          outcome = invoke();
        } catch (const std::exception& e) {
          outcome = JobOutcome::failure(error("am.jobs.handler_exception", Errc::internal, e.what()));
        } catch (const Error& e) {
          outcome = JobOutcome::failure(error("am.jobs.handler_exception", Errc::internal, to_log_string(e)));
        } catch (...) {
          outcome = JobOutcome::failure(error("am.jobs.handler_exception", Errc::internal));
        }
        std::optional<JobProgress> pending;
        {
          const std::lock_guard lock(self->mutex);
          pending = execution->pending;
        }
        if (pending) self->report(execution, *pending, true);
        {
          const std::lock_guard lock(self->mutex);
          if (execution->failure) outcome = JobOutcome::failure(*execution->failure);
          execution->outcome = std::move(outcome);
          execution->done = true;
          ++self->epoch;
        }
        self->changed.notify_all();
      });
    } catch (const std::exception& e) {
      const std::lock_guard lock(mutex);
      execution->outcome.kind = OutcomeKind::resource_error;
      try {
        execution->outcome =
            JobOutcome::failure(error("am.jobs.worker_unavailable", Errc::resource_exhausted, e.what()));
      } catch (...) {
      }
      execution->done = true;
      ++epoch;
      changed.notify_all();
    }
  }
  void complete(const std::shared_ptr<Execution>& execution) {
    std::vector<JobEvent> events;
    {
      const std::lock_guard lock(mutex);
      const auto current = get(execution->record.job_id);
      auto outcome = execution->outcome;
      if (outcome.kind == OutcomeKind::await_children && execution->stop.stop_requested())
        outcome = JobOutcome::interrupted();
      if (outcome.kind == OutcomeKind::await_children) {
        parents[current.job_id] = current.attempt;
        return;
      }
      if (outcome.kind == OutcomeKind::failed && outcome.error) {
        switch (outcome.error->code) {
          case Errc::insufficient_space:
          case Errc::resource_exhausted:
            outcome.kind = OutcomeKind::waiting_resources;
            break;
          case Errc::read_only:
          case Errc::locked:
            outcome.kind = OutcomeKind::cannot_continue_safely;
            break;
          case Errc::cancelled:
            break;
          default:
            break;
        }
      }
      auto state = JobState::failed;
      JobPatch patch;
      if ((outcome.kind == OutcomeKind::failed || outcome.kind == OutcomeKind::needs_decision) &&
          !(outcome.error && outcome.error->code == Errc::cancelled) &&
          !outcome.result_json.empty() && outcome.result_json != "{}") {
        if (json::parse_strict(outcome.result_json))
          patch.result_json = outcome.result_json;
        else
          outcome = JobOutcome::failure(error("am.jobs.json_invalid"));
      }
      switch (outcome.kind) {
        case OutcomeKind::succeeded:
          if (!json::parse_strict(outcome.result_json)) {
            outcome = JobOutcome::failure(error("am.jobs.json_invalid"));
          } else {
            state = JobState::succeeded;
            patch.result_json = outcome.result_json;
          }
          break;
        case OutcomeKind::needs_decision:
          try {
            patch.decision_json = question_json(outcome.decision);
            state = JobState::needs_decision;
          } catch (const Error& e) {
            outcome = JobOutcome::failure(e);
          }
          break;
        case OutcomeKind::waiting_resources:
          if (retry_counts[current.job_id] >= options.auto_retries)
            state = JobState::resource_error;
          else {
            state = JobState::waiting_resources;
            retries[current.job_id] = now() + options.retry_interval;
          }
          break;
        case OutcomeKind::resource_error:
          state = JobState::resource_error;
          break;
        case OutcomeKind::cannot_continue_safely:
          state = JobState::cannot_continue_safely;
          break;
        case OutcomeKind::interrupted:
          switch (execution->reason.load()) {
            case StopReason::pause:
              state = JobState::paused;
              break;
            case StopReason::shutdown:
              state = JobState::queued;
              patch.release_attempt = true;
              break;
            default:
              state = JobState::cancelled;
              break;
          }
          break;
        default:
          if (outcome.error && outcome.error->code == Errc::cancelled) state = JobState::cancelled;
          break;
      }
      patch.clear_decision = settled(state);
      patch.error_json = outcome.error ? detail::error_json(*outcome.error) : "null";
      // A cooperative unit may enqueue children after the control request. Cover
      // these late arrivals as well as the children present when control ran.
      if (!current.parent_id && (state == JobState::paused || state == JobState::cancelled)) {
        JobFilter filter;
        filter.parent_id = current.job_id;
        for (const auto& child : take(store.list(filter))) {
          const bool cancel =
              state == JobState::cancelled && (child.state == JobState::queued || attention(child.state));
          const bool pause = state == JobState::paused && child.state == JobState::queued;
          if (!cancel && !pause) continue;
          JobPatch child_patch;
          child_patch.clear_decision = cancel;
          const auto next =
              transition(child.job_id, child.state, cancel ? JobState::cancelled : JobState::paused, child_patch);
          touched.insert(child.job_id);
          events.push_back({EventKind::state_changed, next, {}});
          events.push_back({EventKind::finished, next, {}});
        }
      }
      const auto record = transition(current.job_id, JobState::running, state, patch);
      events.push_back({EventKind::state_changed, record, {}});
      events.push_back({EventKind::finished, record, {}});
    }
    for (const auto& event : events) emit(event);
  }
  void shutdown() noexcept {
    std::map<std::string, std::shared_ptr<Execution>> workers;
    std::vector<JobEvent> events;
    try {
      {
        const std::lock_guard lock(mutex);
        workers.swap(active);
        for (const auto& [id, execution] : workers) {
          (void)id;
          if (execution->reason.load() == StopReason::none) execution->reason = StopReason::shutdown;
        }
      }
      for (const auto& [id, execution] : workers) {
        (void)id;
        execution->stop.request_stop();
      }
      for (const auto& [id, execution] : workers) {
        (void)id;
        if (execution->worker.joinable()) execution->worker.join();
      }
      for (const auto& [id, execution] : workers) {
        (void)id;
        try {
          complete(execution);
        } catch (...) {
        }
      }
      const std::lock_guard lock(mutex);
      retries.clear();
      for (const auto& [id, attempt] : parents) {
        try {
          const auto record = get(id);
          if (record.state == JobState::running && record.attempt == attempt) {
            JobPatch patch;
            patch.release_attempt = true;
            const auto next = transition(id, JobState::running, JobState::queued, patch);
            events.push_back({EventKind::state_changed, next, {}});
            events.push_back({EventKind::finished, next, {}});
          }
        } catch (...) {
        }
      }
      parents.clear();
      running = false;
    } catch (...) {
    }
    for (const auto& event : events) emit(event);
  }
  Result<RunSummary> run(std::stop_token stop) {
    return guard<RunSummary>([&]() -> Result<RunSummary> {
      take(validate_limits(options.limits));
      if (options.report_interval.count() < 0 || options.retry_interval.count() < 0 ||
          options.poll_interval.count() <= 0 || options.auto_retries < 0)
        return error("am.jobs.limits_invalid");
      struct Cleanup {
        ExecutorState* state;
        bool owned = false, success = false;
        ~Cleanup() {
          if (owned && !success) state->shutdown();
        }
      } cleanup{this};
      {
        const std::lock_guard lock(mutex);
        if (running) return error("am.jobs.state_conflict", Errc::conflict);
        running = true;
        cleanup.owned = true;
        touched.clear();
        retries.clear();
        retry_counts.clear();
        JobFilter waiting;
        waiting.states = {JobState::waiting_resources};
        for (const auto& record : take(store.list(waiting)))
          if (record.state == JobState::waiting_resources && options.auto_retries > 0) {
            (void)transition(record.job_id, record.state, JobState::queued);
            retry_counts[record.job_id] = 1;
          }
      }
      std::stop_callback signal(stop, [this] { changed.notify_all(); });
      for (;;) {
        std::vector<std::shared_ptr<Execution>> completed;
        {
          const std::lock_guard lock(mutex);
          for (auto at = active.begin(); at != active.end();) {
            if (at->second->done) {
              completed.push_back(at->second);
              at = active.erase(at);
            } else
              ++at;
          }
        }
        for (const auto& execution : completed) {
          if (execution->worker.joinable()) execution->worker.join();
          complete(execution);
        }
        if (stop.stop_requested()) {
          shutdown();
          break;
        }
        std::vector<std::pair<std::shared_ptr<Execution>, bool>> launches;
        {
          const std::lock_guard lock(mutex);
          for (auto at = retries.begin(); at != retries.end();) {
            const auto record = get(at->first);
            if (record.state != JobState::waiting_resources)
              at = retries.erase(at);
            else if (at->second <= now()) {
              (void)transition(record.job_id, record.state, JobState::queued);
              ++retry_counts[record.job_id];
              at = retries.erase(at);
            } else
              ++at;
          }
          JobFilter roots;
          roots.states = {JobState::queued, JobState::running};
          roots.top_level_only = true;
          const auto all = take(store.list(roots));
          std::vector<std::pair<JobRecord, bool>> candidates;
          for (const auto& root : all) {
            if (root.parent_id) continue;
            if (root.state == JobState::queued)
              candidates.emplace_back(root, true);
            else if (root.state == JobState::running && parents.contains(root.job_id)) {
              JobFilter child_filter;
              child_filter.parent_id = root.job_id;
              const auto children = take(store.list(child_filter));
              if (summarize_children(children).settled())
                candidates.emplace_back(root, false);
              else
                for (const auto& child : children)
                  if (child.state == JobState::queued) candidates.emplace_back(child, true);
            }
          }
          for (const auto& [candidate, starting] : candidates) {
            if (stop.stop_requested()) break;
            const bool interactive = candidate.priority >= interactive_priority;
            const auto background = std::count_if(active.begin(), active.end(), [](const auto& entry) {
              return entry.second->record.priority < interactive_priority;
            });
            if (active.size() >= static_cast<std::size_t>(options.limits.jobs) + 1 ||
                (!interactive && background >= options.limits.jobs))
              continue;
            auto execution = std::make_shared<Execution>();
            execution->record = candidate;
            if (starting)
              execution->record = transition(candidate.job_id, JobState::queued, JobState::running);
            else
              parents.erase(candidate.job_id);
            touched.insert(candidate.job_id);
            active.emplace(candidate.job_id, execution);
            launches.emplace_back(execution, starting);
          }
        }
        for (const auto& [execution, started] : launches) launch(execution, started);
        if (!launches.empty()) continue;
        std::unique_lock lock(mutex);
        if (std::any_of(active.begin(), active.end(), [](const auto& entry) { return entry.second->done; })) continue;
        if (active.empty() && retries.empty()) break;
        const auto before = epoch;
        auto interval = options.poll_interval;
        if (!retries.empty()) {
          const auto earliest = std::min_element(retries.begin(), retries.end(),
                                                 [](const auto& a, const auto& b) { return a.second < b.second; });
          interval = std::min(
              interval, std::max(std::chrono::milliseconds(0),
                                 std::chrono::duration_cast<std::chrono::milliseconds>(earliest->second - now())));
        }
        changed.wait_for(lock, interval, [&] { return stop.stop_requested() || epoch != before; });
      }
      RunSummary summary;
      summary.interrupted = stop.stop_requested();
      {
        const std::lock_guard lock(mutex);
        summary.processed = touched.size();
        JobFilter remaining;
        remaining.states = {JobState::queued, JobState::running, JobState::paused, JobState::needs_decision,
                            JobState::waiting_resources, JobState::resource_error,
                            JobState::cannot_continue_safely, JobState::failed};
        auto records = take(store.list(remaining));
        for (const auto& id : touched) {
          auto record = get(id);
          if (job_terminal(record.state)) records.push_back(std::move(record));
        }
        std::sort(records.begin(),records.end(),[](const auto& a,const auto& b) {
          if(a.priority!=b.priority) return a.priority>b.priority;
          if(a.position!=b.position) return a.position<b.position;
          return a.job_id<b.job_id;
        });
        for (auto record : records) {
          if (record.state == JobState::succeeded) ++summary.succeeded;
          if (record.state == JobState::failed) ++summary.failed;
          if (attention(record.state)) ++summary.attention;
          summary.jobs.push_back(std::move(record));
        }
        running = false;
      }
      cleanup.success = true;
      return summary;
    });
  }
  Result<void> control(std::string_view id, std::string_view command) {
    return guard<void>([&]() -> Result<void> {
      std::vector<JobEvent> events;
      std::vector<std::shared_ptr<Execution>> stops;
      {
        const std::lock_guard lock(mutex);
        auto apply = [&](const JobRecord& record, bool child) {
          std::optional<JobState> to;
          if (command == "pause" && record.state == JobState::queued) to = JobState::paused;
          if (command == "resume" && (record.state == JobState::paused || record.state == JobState::waiting_resources))
            to = JobState::queued;
          if (command == "retry" && (record.state == JobState::failed || record.state == JobState::resource_error ||
                                     record.state == JobState::cannot_continue_safely))
            to = JobState::queued;
          if (command == "cancel" && (record.state == JobState::queued || attention(record.state)))
            to = JobState::cancelled;
          if (record.state == JobState::running && (command == "pause" || command == "cancel")) {
            const auto at = active.find(record.job_id);
            if (at != active.end()) {
              if (at->second->reason.load() != StopReason::cancel)
                at->second->reason = command == "pause" ? StopReason::pause : StopReason::cancel;
              stops.push_back(at->second);
              return;
            }
            if (parents.contains(record.job_id)) {
              parents.erase(record.job_id);
              to = command == "pause" ? JobState::paused : JobState::cancelled;
            }
          }
          if (!to) {
            if (child) return;
            throw error("am.jobs.transition_invalid");
          }
          JobPatch patch;
          patch.clear_decision = settled(*to);
          const auto next = transition(record.job_id, record.state, *to, patch);
          retries.erase(record.job_id);
          events.push_back({EventKind::state_changed, next, {}});
          if (*to != JobState::queued) events.push_back({EventKind::finished, next, {}});
        };
        const auto root = get(id);
        apply(root, false);
        if (!root.parent_id) {
          JobFilter filter;
          filter.parent_id = root.job_id;
          for (const auto& child : take(store.list(filter))) apply(child, true);
        }
        ++epoch;
      }
      for (const auto& execution : stops) execution->stop.request_stop();
      changed.notify_all();
      for (const auto& event : events) emit(event);
      return {};
    });
  }
};
JobContext::JobContext(std::shared_ptr<ExecutorState> s, std::shared_ptr<Execution> e)
    : state_(std::move(s)), execution_(std::move(e)) {}
const JobRecord& JobContext::record() const noexcept { return execution_->record; }
std::stop_token JobContext::stop() const noexcept { return execution_->stop.get_token(); }
StopReason JobContext::stop_reason() const noexcept { return execution_->reason.load(); }
void JobContext::report(const JobProgress& progress) { state_->report(execution_, progress, false); }
Result<void> JobContext::checkpoint(std::string_view json) {
  return guard<void>([&]() -> Result<void> {
    take(contracts::validate_job_checkpoint(json));
    const std::lock_guard lock(state_->mutex);
    take(state_->store.checkpoint(execution_->record.job_id, json));
    execution_->record.result_json = std::string(json);
    return {};
  });
}
Result<Lease> JobContext::acquire(std::span<const ResourceRequest> requests) {
  return state_->resources.acquire(requests, record().priority >= interactive_priority, record().priority, stop());
}
std::optional<std::string> JobContext::answer() const { return selected_answer(record().decision_json); }
Profile JobContext::profile() const noexcept { return execution_->profile; }
Result<JobRecord> JobContext::enqueue_child(JobSpec spec) {
  return guard<JobRecord>([&]() -> Result<JobRecord> {
    const std::lock_guard lock(state_->mutex);
    if (record().parent_id) return error("am.jobs.parent_invalid");
    spec.parent_id = record().job_id;
    auto result = state_->store.enqueue(spec);
    ++state_->epoch;
    state_->changed.notify_all();
    return result;
  });
}
ChildrenSummary JobContext::children() const {
  const std::lock_guard lock(state_->mutex);
  JobFilter filter;
  filter.parent_id = record().job_id;
  auto children = state_->store.list(filter);
  if (children) return summarize_children(std::move(*children));
  execution_->failure = children.error();
  ChildrenSummary out;
  out.error = children.error();
  return out;
}
Executor::Executor(IJobStore& store, ResourceManager& resources, const HandlerRegistry& registry,
                   ExecutorOptions options, JobObserver* observer)
    : state_(std::make_shared<ExecutorState>(store, resources, registry, std::move(options), observer)) {}
Executor::~Executor() { state_->shutdown(); }
Result<RunSummary> Executor::run_until_idle(std::stop_token stop) { return state_->run(stop); }
Result<void> Executor::pause(std::string_view id) { return state_->control(id, "pause"); }
Result<void> Executor::resume(std::string_view id) { return state_->control(id, "resume"); }
Result<void> Executor::cancel(std::string_view id) { return state_->control(id, "cancel"); }
Result<void> Executor::retry(std::string_view id) { return state_->control(id, "retry"); }
Result<void> Executor::answer(std::string_view id, std::string_view option) {
  return guard<void>([&]() -> Result<void> {
    JobRecord next;
    {
      const std::lock_guard lock(state_->mutex);
      const auto record = state_->get(id);
      if (record.state != JobState::needs_decision) return error("am.jobs.transition_invalid");
      if (!record.decision_json) return error("am.jobs.answer_invalid");
      const auto tree = take(json::parse_strict(*record.decision_json));
      const auto& root = tree.nodes[0];
      const auto* options = detail::field(tree, root, "options");
      bool found = false;
      if (options && options->kind == json::Kind::array)
        for (const auto index : options->children)
          if (tree.nodes[index].kind == json::Kind::string && tree.nodes[index].decoded == option) found = true;
      if (!found) return error("am.jobs.answer_invalid");
      std::string decision = "{";
      for (auto index : root.children) {
        const auto& node = tree.nodes[index];
        if (node.edge == "answer") continue;
        decision +=
            detail::quote(node.edge) + ":" + record.decision_json->substr(node.begin, node.end - node.begin) + ",";
      }
      decision += "\"answer\":{\"option\":" + detail::quote(option) + "}}";
      JobPatch patch;
      patch.decision_json = std::move(decision);
      next = state_->transition(id, record.state, JobState::queued, patch);
    }
    state_->changed.notify_all();
    state_->emit({EventKind::state_changed, next, {}});
    return {};
  });
}
void Executor::wake() noexcept {
  {
    const std::lock_guard lock(state_->mutex);
    ++state_->epoch;
  }
  state_->changed.notify_all();
}
}  // namespace am::jobs
