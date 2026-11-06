#include <gtest/gtest.h>

#include "am/automation/command_registry.hpp"

using namespace am::automation;
using am::contracts::Idempotency;

namespace {
Command make(std::string id, CommandOrigin origin, CommandContext ctx = CommandContext::asset,
             CommandPermission perm = CommandPermission::read) {
  Command c;
  c.id = std::move(id);
  c.title = "t";
  c.origin = origin;
  c.contexts = ctx;
  c.permission = perm;
  c.run = [](const CommandInvocation& inv) -> am::Result<std::string> { return inv.args_json; };
  return c;
}
}  // namespace

TEST(CommandRegistry, RegisterInvokeAndFilter) {
  CommandRegistry r;
  ASSERT_TRUE(r.add(make("am.asset.export", CommandOrigin::builtin)).ok());
  ASSERT_TRUE(r.add(make("am.library.rebuild", CommandOrigin::builtin, CommandContext::library)).ok());
  EXPECT_EQ(r.for_context(CommandContext::asset).size(), 1u);
  auto res = r.invoke("am.asset.export", {R"({"x":1})"});
  ASSERT_TRUE(res.ok());
  EXPECT_EQ(*res, R"({"x":1})");
  EXPECT_FALSE(r.invoke("nope", {}).ok());
}

TEST(CommandRegistry, PluginsCannotReplaceOrSquatBuiltins) {  // ТЗ 10.9.3
  CommandRegistry r;
  ASSERT_TRUE(r.add(make("am.asset.export", CommandOrigin::builtin)).ok());
  EXPECT_FALSE(r.add(make("am.asset.export", CommandOrigin::plugin)).ok());
  EXPECT_FALSE(r.add(make("am.asset.other", CommandOrigin::plugin)).ok());
  EXPECT_TRUE(r.add(make("x.acme.ctb_preview", CommandOrigin::plugin)).ok());
  EXPECT_EQ(r.find("x.acme.ctb_preview")->origin, CommandOrigin::plugin);
}

// ТЗ §11.1.3 / criterion 79: a scenario or LLM with a lower grant is rejected
// before the handler runs, so nothing is written.
TEST(CommandRegistry, GrantedPermissionLevelIsEnforced) {
  CommandRegistry r;
  bool ran = false;
  auto c = make("am.asset.set_tags", CommandOrigin::builtin, CommandContext::asset, CommandPermission::modify);
  c.run = [&](const CommandInvocation&) -> am::Result<std::string> { ran = true; return std::string("{}"); };
  ASSERT_TRUE(r.add(std::move(c)).ok());
  ASSERT_TRUE(r.add(make("am.search", CommandOrigin::builtin, CommandContext::global)).ok());

  auto denied = r.invoke_as("am.asset.set_tags", {}, CommandPermission::prepare);
  ASSERT_FALSE(denied.ok());
  EXPECT_EQ(denied.error().code, am::Errc::permission_denied);
  EXPECT_FALSE(ran);

  EXPECT_TRUE(r.invoke_as("am.search", {}, CommandPermission::read).ok());
  EXPECT_TRUE(r.invoke_as("am.asset.set_tags", {}, CommandPermission::modify).ok());
  EXPECT_TRUE(ran);
}

TEST(CommandRegistry, OpIdRequiredForOpIdIdempotentCommands) {  // ТЗ §11.3.6
  CommandRegistry r;
  auto c = make("am.import.run", CommandOrigin::builtin, CommandContext::global, CommandPermission::modify);
  c.idempotency = Idempotency::by_op_id;
  ASSERT_TRUE(r.add(std::move(c)).ok());
  EXPECT_FALSE(r.invoke("am.import.run", {}).ok());
  CommandInvocation inv;
  inv.op_id = "0192f5e0-0000-7000-8000-000000000001";
  EXPECT_TRUE(r.invoke("am.import.run", inv).ok());
}
