#include "platform_memory.hpp"
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
namespace s7 {
void initialize_process() {
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
  // Expected crash probes must not produce core files outside the build tree.
  const rlimit no_core{0, 0};
  if (setrlimit(RLIMIT_CORE, &no_core)) throw std::runtime_error("cannot disable process core dumps");
#endif
}
Memory memory() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
    throw std::runtime_error("GetProcessMemoryInfo failed");
  return {static_cast<std::uint64_t>(counters.PeakWorkingSetSize), static_cast<std::uint64_t>(counters.PeakPagefileUsage)};
#else
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
  return {static_cast<std::uint64_t>(usage.ru_maxrss), 0};
#else
  return {static_cast<std::uint64_t>(usage.ru_maxrss) * 1024, 0};
#endif
#endif
}
void atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("atomic replacement failed");
#else
  std::filesystem::rename(from, to);
#endif
}
}  // namespace s7
