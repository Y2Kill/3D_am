#include <cstdlib>

#include "am/application/logging.hpp"

// Linked only into the test driver. The production CLI has no environment injection.
void install_diagnostic_cli_test_hook(am::application::DiagnosticOptions& options) {
  std::string selected;
#ifdef _WIN32
  std::size_t size = 0;
  if (getenv_s(&size, nullptr, 0, "AM_TEST_DIAG_PLACEHOLDER") || !size) return;
  selected.resize(size);
  if (getenv_s(&size, selected.data(), selected.size(), "AM_TEST_DIAG_PLACEHOLDER")) return;
  selected.pop_back();
#else
  const auto* name = std::getenv("AM_TEST_DIAG_PLACEHOLDER");
  if (!name || !*name) return;
  selected = name;
#endif
  options.node_data = [selected = std::move(selected)](const auto& path) -> am::Result<am::paths::WindowsNodeData> {
    const auto actual = am::paths::node_info(path);
    if (!actual) return actual.error();
    am::paths::WindowsNodeData raw;
    raw.size = actual->size;
    raw.mtime_ns = actual->mtime_ns;
    if (actual->kind == am::paths::NodeKind::directory) raw.attributes |= 0x10;
    if (actual->kind == am::paths::NodeKind::link) {
      raw.attributes |= 0x400;
      raw.reparse_tag = actual->reparse_tag.value_or(0xa000000c);
    }
    if (path.filename() == std::filesystem::path(selected)) raw.attributes |= 0x400000;
    return raw;
  };
}
