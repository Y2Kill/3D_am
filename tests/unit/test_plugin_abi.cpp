// Compile-level checks of the draft plugin ABI (ADR-0026). The header must be
// self-contained, C-compatible and independent of internal am/ headers.
#include <gtest/gtest.h>

#include <type_traits>

#include "am/am_plugin.h"

static_assert(std::is_standard_layout_v<am_plugin_v1>);
static_assert(std::is_trivially_copyable_v<am_command_v1>);
static_assert(offsetof(am_plugin_v1, struct_size) == 0, "struct_size must be the first field");
static_assert(offsetof(am_command_v1, struct_size) == 0, "struct_size must be the first field");

TEST(PluginAbi, PermissionOrderMatchesCoreContract) {
  EXPECT_LT(AM_PERM_READ, AM_PERM_PREPARE);
  EXPECT_LT(AM_PERM_PREPARE, AM_PERM_MODIFY);
  am_plugin_v1 p{};
  p.struct_size = sizeof p;
  EXPECT_EQ(p.on_drain, nullptr);  // appended field defaults to "not provided"
}
