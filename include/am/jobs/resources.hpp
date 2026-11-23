#pragma once
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "am/foundation/result.hpp"

namespace am::jobs {
inline constexpr int interactive_priority = 100;
enum class ResourceKind { cpu, read, write, external_process };
enum class MediaKind { hdd, ssd, network, removable, unknown };
enum class Profile { economy, balanced, performance, custom };
struct ResourceRequest {
  ResourceKind kind;
  std::string device;
};
struct ResourceLimits {
  unsigned jobs = 2, cpu = 1, external_process = 1;
  unsigned io_hdd = 1, io_ssd = 2, io_network = 2, io_removable = 1, io_unknown = 1;
};
// All profile numbers are hypotheses, not performance requirements.
ResourceLimits profile_limits(Profile, unsigned hardware_concurrency) noexcept;
Result<void> validate_limits(const ResourceLimits&);
struct ResourceUsage {
  ResourceKind kind;
  std::string device;
  unsigned occupied, limit;
};
struct ResourceWaiter {
  bool interactive;
  int priority;
  std::uint64_t sequence;
  std::vector<ResourceRequest> resources;
};
struct ResourceSnapshot {
  std::vector<ResourceUsage> usage;
  std::vector<ResourceWaiter> waiting;
};
struct ResourceState;
class Lease {
 public:
  Lease(Lease&&) noexcept;
  Lease& operator=(Lease&&) noexcept;
  ~Lease();
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;

 private:
  struct Held;
  std::unique_ptr<Held> held_;
  explicit Lease(std::unique_ptr<Held>);
  friend class ResourceManager;
};
class ResourceManager {
 public:
  // Optional waiter-registration observer, invoked without resource locks.
  explicit ResourceManager(ResourceLimits, std::function<void()> on_waiting = {});
  ~ResourceManager();
  void set_media(std::string device, MediaKind);
  Result<Lease> acquire(std::span<const ResourceRequest>, bool interactive, int priority, std::stop_token = {});
  ResourceSnapshot snapshot() const;

 private:
  std::shared_ptr<ResourceState> state_;
};
}  // namespace am::jobs
