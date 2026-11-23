#pragma once
#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>

#include "am/foundation/json_text.hpp"
#include "am/foundation/log.hpp"
namespace log_tests {
struct MemorySink final : am::log::Sink {
  am::log::Level minimum = am::log::Level::trace;
  std::mutex mutex;
  std::vector<std::string> rows;
  std::vector<std::thread::id> threads;
  std::atomic<unsigned> writes = 0;
  am::log::Level min_level() const noexcept override { return minimum; }
  void write(const am::log::Record& record) noexcept override {
    try {
      const std::lock_guard lock(mutex);
      rows.push_back(am::log::serialize(record));
      threads.push_back(std::this_thread::get_id());
      ++writes;
    } catch (...) {
    }
  }
};
struct CounterSink final : am::log::Sink {
  std::atomic<std::uint64_t> count = 0;
  am::log::Level min_level() const noexcept override { return am::log::Level::trace; }
  void write(const am::log::Record&) noexcept override { ++count; }
};
inline am::json::Tree parse(std::string_view value) {
  auto result = am::json::parse_strict(value);
  if (!result) throw std::runtime_error("Invalid log JSON");
  return std::move(*result);
}
inline const am::json::Node& field(const am::json::Tree& tree, const am::json::Node& parent, std::string_view key) {
  for (auto i : parent.children)
    if (tree.nodes[i].edge == key) return tree.nodes[i];
  throw std::runtime_error("Missing log field");
}
inline std::string context(std::string_view row, std::string_view key) {
  auto tree = parse(row);
  return field(tree, field(tree, tree.nodes[0], "ctx"), key).decoded;
}
}  // namespace log_tests
