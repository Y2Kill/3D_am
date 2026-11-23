#include "am/jobs/resources.hpp"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace am::jobs {
namespace {
Error error(std::string id, Errc code = Errc::invalid_argument) {
  return Error::make(code, "Job resource operation failed").in("jobs.resources").id(std::move(id));
}
using Key = std::pair<ResourceKind, std::string>;
Key key(const ResourceRequest& r) {
  return {r.kind, r.kind == ResourceKind::cpu || r.kind == ResourceKind::external_process ? "" : r.device};
}
}  // namespace
ResourceLimits profile_limits(Profile profile, unsigned hc) noexcept {
  hc = std::max(1u, hc);
  if (profile == Profile::economy) return {1, 1, 1, 1, 1, 1, 1, 1};
  if (profile == Profile::performance) return {std::max(2u, hc / 2), std::max(1u, hc - 1), 2, 1, 4, 2, 1, 2};
  return {2, std::max(1u, hc / 2), 1, 1, 2, 2, 1, 1};
}
Result<void> validate_limits(const ResourceLimits& l) {
  if (!l.jobs || !l.cpu || !l.external_process || !l.io_hdd || !l.io_ssd || !l.io_network || !l.io_removable ||
      !l.io_unknown)
    return error("am.jobs.limits_invalid");
  return {};
}
struct ResourceState {
  ResourceLimits limits;
  mutable std::mutex mutex;
  std::condition_variable_any changed;
  std::map<std::string, MediaKind> media;
  std::map<Key, unsigned> used;
  std::map<std::thread::id, std::map<ResourceKind, unsigned>> owners;
  struct Wait {
    ResourceWaiter value;
    std::thread::id owner;
  };
  std::vector<std::shared_ptr<Wait>> waiting;
  std::uint64_t next = 0;
  std::function<void()> on_waiting;
  ResourceState(ResourceLimits l, std::function<void()> observer) : limits(l), on_waiting(std::move(observer)) {}
  unsigned limit(const Key& k) const {
    if (k.first == ResourceKind::cpu) return limits.cpu;
    if (k.first == ResourceKind::external_process) return limits.external_process;
    const auto at = media.find(k.second);
    const auto kind = at == media.end() ? MediaKind::unknown : at->second;
    switch (kind) {
      case MediaKind::hdd:
        return limits.io_hdd;
      case MediaKind::ssd:
        return limits.io_ssd;
      case MediaKind::network:
        return limits.io_network;
      case MediaKind::removable:
        return limits.io_removable;
      default:
        return limits.io_unknown;
    }
  }
};
struct Lease::Held {
  std::shared_ptr<ResourceState> state;
  std::thread::id owner;
  std::vector<Key> keys;
  ~Held() {
    if (!state) return;
    {
      const std::lock_guard lock(state->mutex);
      for (const auto& k : keys) {
        --state->used[k];
        auto& kinds = state->owners.at(owner);
        if (--kinds.at(k.first) == 0) kinds.erase(k.first);
        if (kinds.empty()) state->owners.erase(owner);
      }
    }
    state->changed.notify_all();
  }
};
Lease::Lease(std::unique_ptr<Held> h) : held_(std::move(h)) {}
Lease::Lease(Lease&&) noexcept = default;
Lease& Lease::operator=(Lease&&) noexcept = default;
Lease::~Lease() = default;
ResourceManager::ResourceManager(ResourceLimits l, std::function<void()> observer)
    : state_(std::make_shared<ResourceState>(l, std::move(observer))) {}
ResourceManager::~ResourceManager() = default;
void ResourceManager::set_media(std::string device, MediaKind kind) {
  {
    const std::lock_guard lock(state_->mutex);
    state_->media[std::move(device)] = kind;
  }
  state_->changed.notify_all();
}
Result<Lease> ResourceManager::acquire(std::span<const ResourceRequest> requests, bool interactive, int priority,
                                       std::stop_token stop) {
  try {
    auto valid = validate_limits(state_->limits);
    if (!valid) return valid.error();
    auto held = std::make_unique<Lease::Held>();
    held->owner = std::this_thread::get_id();
    std::set<Key> unique;
    for (const auto& r : requests) {
      if (r.kind != ResourceKind::cpu && r.kind != ResourceKind::read && r.kind != ResourceKind::write &&
          r.kind != ResourceKind::external_process)
        return error("am.jobs.request_invalid");
      unique.insert(key(r));
    }
    held->keys.assign(unique.begin(), unique.end());
    std::unique_lock lock(state_->mutex);
    const auto owner = state_->owners.find(held->owner);
    if (owner != state_->owners.end())
      for (const auto& k : held->keys)
        if (const auto found = owner->second.find(k.first); found != owner->second.end() && found->second)
          return error("am.jobs.lease_nested");
    auto waiter = std::make_shared<ResourceState::Wait>();
    waiter->owner = held->owner;
    waiter->value = {interactive, priority, state_->next++, {}};
    for (const auto& k : held->keys) waiter->value.resources.push_back({k.first, k.second});
    state_->waiting.push_back(waiter);
    struct Registration {
      ResourceState& state;
      std::shared_ptr<ResourceState::Wait> waiter;
      bool active = true;
      void remove() noexcept {
        if (active) {
          std::erase(state.waiting, waiter);
          state.changed.notify_all();
          active = false;
        }
      }
      ~Registration() { remove(); }
    } registration{*state_, waiter};
    if (state_->on_waiting) {
      lock.unlock();
      try {
        state_->on_waiting();
      } catch (...) {
      }
      lock.lock();
    }
    auto remove = [&] { registration.remove(); };
    auto ready = [&] {
      const auto first =
          std::min_element(state_->waiting.begin(), state_->waiting.end(), [](const auto& a, const auto& b) {
            if (a->value.interactive != b->value.interactive) return a->value.interactive;
            if (a->value.priority != b->value.priority) return a->value.priority > b->value.priority;
            return a->value.sequence < b->value.sequence;
          });
      if (first == state_->waiting.end() || *first != waiter) return false;
      for (const auto& k : held->keys)
        if (state_->used[k] >= state_->limit(k)) return false;
      return true;
    };
    if (!state_->changed.wait(lock, stop, ready) || stop.stop_requested()) {
      remove();
      return error("am.jobs.resource_cancelled", Errc::cancelled);
    }
    // Allocate accounting nodes before changing counters, so allocation failure cannot leak a partial lease.
    auto& kinds = state_->owners[held->owner];
    for (const auto& k : held->keys) {
      state_->used.try_emplace(k, 0);
      kinds.try_emplace(k.first, 0);
    }
    for (const auto& k : held->keys) {
      ++state_->used[k];
      ++kinds[k.first];
    }
    remove();
    held->state = state_;
    return Lease(std::move(held));
  } catch (const std::bad_alloc&) {
    return error("am.jobs.out_of_memory", Errc::resource_exhausted);
  } catch (...) {
    return error("am.jobs.resource_error", Errc::internal);
  }
}
ResourceSnapshot ResourceManager::snapshot() const {
  const std::lock_guard lock(state_->mutex);
  ResourceSnapshot out;
  for (const auto& [k, n] : state_->used) out.usage.push_back({k.first, k.second, n, state_->limit(k)});
  for (const auto& w : state_->waiting) out.waiting.push_back(w->value);
  std::sort(out.waiting.begin(), out.waiting.end(), [](const auto& a, const auto& b) {
    if (a.interactive != b.interactive) return a.interactive;
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.sequence < b.sequence;
  });
  return out;
}
}  // namespace am::jobs
