#pragma once
#include <algorithm>
#include <atomic>
#include <mutex>

#include "am/contracts/job_store.hpp"
#include "am/foundation/json_text.hpp"
#include "am/foundation/uuid.hpp"

namespace job_tests {
namespace c = am::contracts;
inline am::Error error(std::string id) {
  return am::Error::make(am::Errc::invalid_argument, "Test job store operation failed").id(std::move(id));
}
class MemoryStore final : public c::IJobStore {
 public:
  am::Result<c::JobRecord> enqueue(const c::JobSpec& spec) override {
    const std::lock_guard lock(mutex_);
    if (!am::json::parse_strict(spec.params_json)) return error("am.jobs.json_invalid");
    const auto id = spec.job_id.value_or(am::Uuid::v4().to_string());
    if (find(id) != records_.end()) return error("am.jobs.exists");
    if (spec.parent_id) {
      const auto parent = find(*spec.parent_id);
      if (parent == records_.end() || parent->parent_id || c::job_terminal(parent->state))
        return error("am.jobs.parent_invalid");
    }
    c::JobRecord r;
    r.job_id = id;
    r.type = spec.type;
    r.params_json = spec.params_json;
    r.parent_id = spec.parent_id;
    r.priority = spec.priority;
    for (const auto& old : records_)
      if (old.parent_id == r.parent_id) r.position = std::max(r.position, old.position);
    ++r.position;
    r.created_at = "2026-11-23T00:00:00Z";
    records_.push_back(r);
    return r;
  }
  am::Result<std::optional<c::JobRecord>> get(std::string_view id) override {
    const std::lock_guard lock(mutex_);
    const auto at = find(id);
    return at == records_.end() ? std::optional<c::JobRecord>{} : std::optional(*at);
  }
  am::Result<std::vector<c::JobRecord>> list(const c::JobFilter& filter = {}) override {
    const std::lock_guard lock(mutex_);
    std::vector<c::JobRecord> result;
    for (const auto& r : records_) {
      if (filter.parent_id && r.parent_id != filter.parent_id) continue;
      if (filter.top_level_only && r.parent_id) continue;
      if (!filter.states.empty() &&
          std::find(filter.states.begin(), filter.states.end(), r.state) == filter.states.end())
        continue;
      result.push_back(r);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
      if (a.priority != b.priority) return a.priority > b.priority;
      if (a.position != b.position) return a.position < b.position;
      return a.job_id < b.job_id;
    });
    return result;
  }
  am::Result<c::JobRecord> transition(std::string_view id, c::JobState from, c::JobState to,
                                      const c::JobPatch& patch = {}) override {
    const std::lock_guard lock(mutex_);
    const auto at = find(id);
    if (!c::job_transition_allowed(from, to) ||
        (patch.release_attempt && (from != c::JobState::running || to != c::JobState::queued)))
      return error("am.jobs.transition_invalid");
    if (at == records_.end() || at->state != from) return error("am.jobs.state_conflict");
    if (to == c::JobState::needs_decision && (!patch.decision_json || patch.clear_decision))
      return error("am.jobs.transition_invalid");
    for (const auto* value : {&patch.decision_json, &patch.result_json, &patch.error_json})
      if (*value && !am::json::parse_strict(**value)) return error("am.jobs.json_invalid");
    at->state = to;
    if (to == c::JobState::running) {
      ++at->attempt;
      at->started_at = at->created_at;
      at->finished_at.reset();
    }
    if (patch.release_attempt) at->attempt = std::max(0, at->attempt - 1);
    if (c::job_terminal(to) || to == c::JobState::failed) at->finished_at = at->created_at;
    if (patch.clear_decision || from == c::JobState::needs_decision)
      at->decision_json = patch.decision_json;
    else if (patch.decision_json)
      at->decision_json = patch.decision_json;
    if (patch.result_json) at->result_json = patch.result_json;
    if (patch.error_json) at->error_json = patch.error_json;
    return *at;
  }
  am::Result<void> report(std::string_view id, const c::JobProgress& progress) override {
    const std::lock_guard lock(mutex_);
    const auto at = find(id);
    if (at == records_.end() || at->state != c::JobState::running) return error("am.jobs.transition_invalid");
    if (progress.done && progress.total && *progress.done > *progress.total) return error("am.jobs.progress_invalid");
    at->progress = progress;
    return {};
  }
  am::Result<void> move_before(std::string_view id, std::optional<std::string_view> before) override {
    const std::lock_guard lock(mutex_);
    const auto at = find(id);
    if (at == records_.end() || (at->state != c::JobState::queued && at->state != c::JobState::paused))
      return error("am.jobs.transition_invalid");
    if (before) {
      const auto target = find(*before);
      if (target == records_.end() || target->parent_id != at->parent_id) return error("am.jobs.parent_invalid");
    }
    if (before && *before == id) return {};
    std::vector<c::JobRecord*> siblings;
    for (auto& r : records_)
      if (r.parent_id == at->parent_id && r.job_id != id) siblings.push_back(&r);
    std::sort(siblings.begin(), siblings.end(), [](auto* a, auto* b) { return a->position < b->position; });
    auto where = before ? std::find_if(siblings.begin(), siblings.end(), [&](auto* r) { return r->job_id == *before; })
                        : siblings.end();
    siblings.insert(where, &*at);
    std::int64_t position = 0;
    for (auto* r : siblings) r->position = ++position;
    return {};
  }
  am::Result<std::size_t> remove_finished(std::string_view before) override {
    const std::lock_guard lock(mutex_);
    return std::erase_if(records_, [&](const auto& r) {
      return c::job_terminal(r.state) && r.finished_at && *r.finished_at < before && !r.parent_id;
    });
  }

 private:
  std::recursive_mutex mutex_;
  std::vector<c::JobRecord> records_;
  std::vector<c::JobRecord>::iterator find(std::string_view id) {
    return std::find_if(records_.begin(), records_.end(), [&](const auto& r) { return r.job_id == id; });
  }
};
class CountingStore final : public c::IJobStore {
 public:
  explicit CountingStore(c::IJobStore& store) : store_(store) {}
  am::Result<c::JobRecord> enqueue(const c::JobSpec& s) override { return store_.enqueue(s); }
  am::Result<std::optional<c::JobRecord>> get(std::string_view id) override { return store_.get(id); }
  am::Result<std::vector<c::JobRecord>> list(const c::JobFilter& f = {}) override { return store_.list(f); }
  am::Result<c::JobRecord> transition(std::string_view id, c::JobState a, c::JobState b,
                                      const c::JobPatch& p = {}) override {
    return store_.transition(id, a, b, p);
  }
  am::Result<void> report(std::string_view id, const c::JobProgress& p) override {
    ++reports;
    return store_.report(id, p);
  }
  am::Result<void> move_before(std::string_view id, std::optional<std::string_view> next) override {
    return store_.move_before(id, next);
  }
  am::Result<std::size_t> remove_finished(std::string_view at) override { return store_.remove_finished(at); }
  std::atomic<unsigned> reports{0};

 private:
  c::IJobStore& store_;
};
}  // namespace job_tests
