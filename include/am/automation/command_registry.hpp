#pragma once
// Single command registry (ТЗ §10.9.4, §12.4; ADR-0017, ADR-0024, ADR-0027).
// Main menu, context menus, toolbars, hotkeys, CLI verbs, the scenario engine
// and the optional LLM adapter all enumerate commands from here.

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "am/contracts/command.hpp"

namespace am::automation {

using contracts::Command;
using contracts::CommandContext;
using contracts::CommandInvocation;
using contracts::CommandOrigin;
using contracts::CommandPermission;

class CommandRegistry final : public contracts::ICommandSink {
 public:
  // Plugins may not replace built-in commands (ТЗ 10.9.3).
  Result<void> add(Command cmd) override;
  const Command* find(std::string_view id) const;
  std::vector<const Command*> for_context(CommandContext ctx) const;
  std::vector<const Command*> all() const;

  // Interactive invocation by the user: full rights.
  Result<std::string> invoke(std::string_view id, const CommandInvocation& inv) const;
  // Invocation on behalf of a scenario/LLM with a granted maximum level
  // (ТЗ §11.1.3, §11.3.5). Rejected without side effects if not allowed.
  Result<std::string> invoke_as(std::string_view id, const CommandInvocation& inv,
                                CommandPermission granted) const;

 private:
  mutable std::mutex m_;
  std::map<std::string, std::unique_ptr<Command>, std::less<>> commands_;
};

}  // namespace am::automation
